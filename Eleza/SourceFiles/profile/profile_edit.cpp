#include "profile/profile_edit.h"

#include "api/api_client_config.h"
#include "boxes/photo_crop_box.h"
#include "data/data_image_decode.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "profile/profile_boxes.h"
#include "profile/profile_common.h"
#include "support/error_text.h"
#include "support/file_dialogs.h"
#include "support/media_image.h"
#include "ui/effects/radial_animation.h"
#include "ui/image/userpic.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/top_bar.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_profile.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QThreadPool>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QKeyEvent>

namespace Profile {
namespace {

constexpr auto kDefaultNameMax = 50;
constexpr auto kDefaultBioMax = 160;
constexpr auto kMaxDroppedSide = 4096;

} // namespace

// The round photo: the current avatar, a freshly chosen picture with the
// upload progress over it, or the initial when the photo is being removed.
class EditSection::Photo final : public Ui::AbstractButton {
public:
	Photo(QWidget *parent, Main::Session *session)
	: AbstractButton(parent)
	, _session(session)
	, _progress([this] { update(); }) {
		resize(st::profileEditPhoto, st::profileEditPhoto);
	}

	void setUser(const Data::User &user) {
		_user = user;
		_source = Support::UserpicSource(_session, user);
		QObject::disconnect(_connection);
		if (_source) {
			_connection = QObject::connect(
				_source.get(),
				&Ui::ImageSource::updated,
				this,
				[this] {
					_current = QImage();
					update();
				});
		}
		_current = QImage();
		update();
	}
	// A null image shows the current avatar again.
	void setImage(const QImage &image) {
		_image = image;
		_prepared = QImage();
		update();
	}
	void setCleared(bool cleared) {
		_cleared = cleared;
		update();
	}
	void setUploading(bool uploading, double progress = 0.) {
		if (_uploading != uploading) {
			_uploading = uploading;
			if (uploading) {
				_progress.start();
			} else {
				_progress.stop();
			}
		}
		_progress.setProgress(progress);
		update();
	}

	int naturalWidth() const override {
		return st::profileEditPhoto;
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return st::profileEditPhoto;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto size = int(st::profileEditPhoto);
		const auto circle = QRect((width() - size) / 2, 0, size, size);
		const auto ratio = devicePixelRatioF();
		if (_ratio != ratio) {
			_ratio = ratio;
			_prepared = _current = QImage();
		}
		auto painted = false;
		if (!_image.isNull()) {
			if (_prepared.isNull()) {
				_prepared = Ui::PrepareUserpic(_image, size, ratio);
			}
			p.drawImage(circle, _prepared);
			painted = true;
		} else if (!_cleared && _source) {
			if (_current.isNull()) {
				const auto image = _source->request(QSize(size, size) * ratio);
				if (!image.isNull()) {
					_current = Ui::PrepareUserpic(image, size, ratio);
				}
			}
			if (!_current.isNull()) {
				p.drawImage(circle, _current);
				painted = true;
			}
		}
		if (!painted) {
			Ui::PaintUserpicPlaceholder(p, circle, _user.id, _user.displayName);
		}
		if (_uploading) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::profileEditPhotoShade);
			p.drawEllipse(circle);
			const auto inset = (size - int(st::profileEditPhotoRing)) / 2;
			_progress.draw(
				p,
				circle.adjusted(inset, inset, -inset, -inset),
				3.,
				st::staticWhite->c);
			return;
		}
		if (isOver()) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::profileEditPhotoShade);
			p.drawEllipse(circle);
			st::profileEditCameraOverIcon.paintInCenter(p, circle);
			return;
		}
		const auto badge = int(st::profileEditBadge);
		const auto badgeRect = style::rtlrect(
			circle.x() + size - badge,
			size - badge,
			badge,
			badge,
			width());
		p.setPen(QPen(st::bgCanvas->c, 2.));
		p.setBrush(st::brandPrimary);
		p.drawEllipse(badgeRect);
		st::profileEditCameraIcon.paintInCenter(p, badgeRect);
	}

	void onStateChanged(State was, StateChangeSource source) override {
		AbstractButton::onStateChanged(was, source);
		update();
	}

private:
	Main::Session *_session = nullptr;
	Data::User _user;
	std::shared_ptr<Ui::ImageSource> _source;
	QMetaObject::Connection _connection;
	QImage _image;
	QImage _prepared;
	QImage _current;
	double _ratio = 0.;
	bool _cleared = false;
	bool _uploading = false;
	Ui::RadialProgressAnimation _progress;

};

Window::SectionFactory EditFactory() {
	return [](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new EditSection(parent, controller));
	};
}

EditSection::EditSection(
	QWidget *parent,
	Window::SessionController *controller)
: SectionWidget(parent, controller)
, _bar(Ui::CreateChild<Ui::TopBar>(this))
, _scroll(Ui::CreateChild<Ui::ScrollArea>(this))
, _checker(this) {
	setTopBar(_bar);
	setAcceptDrops(true);
	_bar->setTitle(tr::lng_profile_edit_profile(tr::now));
	const auto &config = controller->session().account().clientConfig();
	_nameMax = (config.displayNameMaxChars() > 0)
		? config.displayNameMaxChars()
		: kDefaultNameMax;
	_bioMax = (config.bioMaxChars() > 0) ? config.bioMaxChars() : kDefaultBioMax;
	build();

	auto &data = controller->session().data();
	_checker.setRequest([this](
			const QString &handle,
			Fn<void(HandleChecker::Answer)> done) {
		this->controller()->session().data().graph().handleAvailable(
			handle,
			this,
			[=](bool available) {
				done(available
					? HandleChecker::Answer::Free
					: HandleChecker::Answer::Taken);
			},
			[=](const Api::Error &) { done(HandleChecker::Answer::Failed); });
	});
	_checker.setChanged([this] {
		_handleError = QString();
		refreshHandleHint();
		refreshState();
	});

	const auto &me = data.users().me();
	if (me.valid()) {
		fill(me);
	}
	// One fetch for fresh values; the form is filled once and then belongs
	// to the user (a later `meChanged` must not overwrite what was typed).
	data.users().refreshMe(this, [this](const Data::Profile &profile) {
		if (!_filled || !dirty()) {
			fill(profile);
		}
	}, [this](const Api::Error &error) {
		if (!_filled) {
			Support::ShowError(
				this->controller(),
				error,
				tr::lng_profile_edit_load_failed(tr::now));
		}
	});
	loadSettings();
	refreshState();
}

EditSection::~EditSection() {
	cancelUpload();
}

QString EditSection::sectionKey() const {
	return u"profile/edit"_s;
}

void EditSection::setInnerFocus() {
	_name->setFocusFast();
}

void EditSection::build() {
	_wrap = _scroll->setOwnedWidget(
		Ui::object_ptr<CenteredWrap>(_scroll, st::profileEditWidth));
	_layout = _wrap->setChild(Ui::object_ptr<Ui::VerticalLayout>(_wrap));
	const auto layout = _layout;
	const auto session = &controller()->session();

	_photo = layout->add(
		Ui::object_ptr<Photo>(layout, session),
		st::profileEditPhotoMargin,
		style::al_top);
	_changePhoto = layout->add(
		Ui::object_ptr<Ui::LinkButton>(
			layout,
			tr::lng_profile_change_photo(tr::now),
			st::profileEditPhotoLink),
		st::profileEditPhotoLinkMargin,
		style::al_top);
	_photoHint = layout->add(
		Ui::object_ptr<Ui::FlatLabel>(layout, st::profileEditHintCentered),
		st::profileEditPhotoHintMargin);

	const auto caption = [&](const QString &text) {
		layout->add(
			Ui::object_ptr<Ui::FlatLabel>(layout, text, st::profileEditCaption),
			st::profileEditCaptionMargin);
	};
	const auto hint = [&] {
		return layout->add(
			Ui::object_ptr<Ui::FlatLabel>(layout, st::profileEditHint),
			st::profileEditHintMargin);
	};

	caption(tr::lng_name(tr::now));
	_name = layout->add(
		Ui::object_ptr<Ui::InputField>(layout, st::profileEditField),
		st::profileEditFieldMargin);
	_name->setMaxLength(_nameMax);
	_nameHint = hint();

	caption(tr::lng_username(tr::now));
	_handle = layout->add(
		Ui::object_ptr<Ui::InputField>(layout, st::profileEditField),
		st::profileEditFieldMargin);
	_handle->setForcedDirection(Qt::LeftToRight);
	_handle->setMaxLength(32);
	_handleHint = hint();

	caption(tr::lng_profile_link(tr::now));
	_link = layout->add(
		Ui::object_ptr<Ui::InputField>(layout, st::profileEditField),
		st::profileEditFieldMargin);
	_link->setForcedDirection(Qt::LeftToRight);
	_linkHint = hint();

	caption(tr::lng_profile_about_me(tr::now));
	_bio = layout->add(
		Ui::object_ptr<Ui::InputField>(
			layout,
			st::profileEditBioField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_profile_edit_bio_placeholder(tr::now)),
		st::profileEditFieldMargin);
	_bio->setSubmitSettings(Ui::InputField::SubmitSettings::CtrlEnter);
	_bioCounter = hint();

	caption(tr::lng_profile_birthday(tr::now));
	_birthdayRow = layout->add(
		Ui::object_ptr<Ui::SettingsButton>(
			layout,
			QString(),
			st::profileEditRow),
		st::profileEditRowMargin);
	_birthdayRow->setChevron(true);
	_birthdayHint = hint();

	_province = layout->add(
		Ui::object_ptr<Ui::SettingsButton>(
			layout,
			tr::lng_profile_edit_show_province(tr::now),
			st::profileEditRow),
		st::profileEditRowMargin);
	_province->setToggle(false);
	_provinceHint = hint();
	layout->add(Ui::object_ptr<Ui::FixedHeightWidget>(layout, st::space2xl));

	_save = Ui::CreateChild<Ui::RoundButton>(
		this,
		tr::lng_eli_save(tr::now),
		st::defaultRoundButton);
	_cancel = Ui::CreateChild<Ui::RoundButton>(
		this,
		tr::lng_cancel(tr::now),
		st::defaultFlatButton);

	_photo->setClickedCallback([this] { showPhotoMenu(); });
	_changePhoto->setClickedCallback([this] { showPhotoMenu(); });
	connect(_name, &Ui::InputField::changed, this, [this] {
		setHint(_nameHint, QString(), false);
		refreshState();
	});
	connect(_handle, &Ui::InputField::changed, this, [this] {
		const auto typed = _handle->text();
		const auto clean = NormalizeHandle(typed);
		if (typed != clean) {
			// Capitals, the "@" and Persian digits never stay in the field.
			_handle->setText(clean);
			_handle->setCursorToEnd();
			return;
		}
		_handleError = QString();
		_checker.setValue(clean);
		refreshHandleHint();
		refreshState();
	});
	connect(_link, &Ui::InputField::changed, this, [this] {
		refreshLinkHint();
		refreshState();
	});
	connect(_bio, &Ui::InputField::changed, this, [this] {
		refreshBioCounter();
		refreshState();
	});
	const auto next = [this](Ui::InputField *from, Ui::InputField *to) {
		connect(from, &Ui::InputField::submitted, this, [=, this](
				Qt::KeyboardModifiers modifiers) {
			if (modifiers & Qt::ControlModifier) {
				save();
			} else {
				to->setFocusFast();
			}
		});
	};
	next(_name, _handle);
	next(_handle, _link);
	next(_link, _bio);
	connect(_bio, &Ui::InputField::submitted, this, [this] { save(); });
	for (const auto field : { _name, _handle, _link, _bio }) {
		connect(field, &Ui::InputField::focusedChanged, this, [=, this](
				bool focused) {
			if (focused) {
				_scroll->scrollToWidget(field);
			}
		});
	}
	_birthdayRow->setClickedCallback([this] {
		const auto weak = QPointer<EditSection>(this);
		this->controller()->show(Ui::Box<BirthdayBox>(
			_birthday,
			[=](std::optional<Data::Birthday> value) {
				if (weak) {
					weak->_birthday = value;
					weak->_birthdayError = QString();
					weak->refreshBirthday();
					weak->refreshState();
				}
			}));
	});
	connect(
		_province,
		&Ui::SettingsButton::toggledChanged,
		this,
		[this](bool checked) { toggleProvince(checked); });
	_save->setClickedCallback([this] { save(); });
	_cancel->setClickedCallback([this] {
		if (!onBackRequest()) {
			this->controller()->showBack();
		}
	});

	refreshBioCounter();
	refreshBirthday();
	setHint(
		_provinceHint,
		tr::lng_profile_edit_province_hint(tr::now),
		false);
}

void EditSection::fill(const Data::Profile &profile) {
	_base = profile;
	_filled = true;
	_checker.setCurrent(profile.handle);
	_photo->setUser(profile);
	_name->setText(profile.displayName);
	_handle->setText(profile.handle);
	_link->setText(DisplayLink(profile.link));
	_bio->setText(profile.bio);
	_birthday = profile.birthday;
	_checker.setValue(profile.handle);
	refreshHandleHint();
	refreshLinkHint();
	refreshBioCounter();
	refreshBirthday();
	refreshPhoto();
	refreshState();
}

void EditSection::loadSettings() {
	auto &graph = controller()->session().data().graph();
	graph.loadSettings(false, this, [this](const QJsonObject &) {
		auto &graph = this->controller()->session().data().graph();
		const auto shown = graph.settingValue(
			u"discovery.show_province_on_profile"_s).toBool();
		const auto mode = graph.settingValue(
			u"discovery.region_mode"_s).toString();
		const auto code = graph.settingValue(
			u"discovery.province_code"_s).toString();
		_province->setToggle(shown);
		setHint(
			_provinceHint,
			(!mode.isEmpty() && mode != u"off" && !code.isEmpty())
				? ProvinceTitle(code)
				: tr::lng_profile_edit_province_hint(tr::now),
			false);
	});
}

void EditSection::toggleProvince(bool on) {
	if (_provinceBusy) {
		return;
	}
	auto &graph = controller()->session().data().graph();
	const auto code = graph.settingValue(
		u"discovery.province_code"_s).toString();
	if (on && code.isEmpty()) {
		// Nothing to show until the region is known.
		_province->setToggle(false, true);
		controller()->showToast(tr::lng_profile_edit_province_enable(tr::now));
		return;
	}
	_provinceBusy = true;
	graph.patchSettings(
		Data::Graph::SettingPatch(
			u"discovery.show_province_on_profile"_s,
			on),
		this,
		[this](const QJsonObject &) { _provinceBusy = false; },
		[=, this](const Api::Error &error) {
			_provinceBusy = false;
			_province->setToggle(!on, true);
			Support::ShowError(
				this->controller(),
				error,
				tr::lng_settings_couldnt_save_try_again(tr::now));
		});
}

void EditSection::setHint(
		Ui::FlatLabel *label,
		const QString &text,
		bool error,
		bool ok) {
	label->setText(text);
	// An empty hint takes no room in the form.
	label->setVisible(!text.isEmpty());
	label->setTextColorOverride(error
		? std::make_optional(st::statusError->c)
		: ok
		? std::make_optional(st::statusSuccess->c)
		: std::nullopt);
}

void EditSection::refreshHandleHint() {
	if (!_handleError.isEmpty()) {
		setHint(_handleHint, _handleError, true);
		return;
	}
	const auto state = _checker.state();
	auto text = QString();
	switch (state) {
	case HandleState::Idle: break;
	case HandleState::Invalid:
		text = tr::lng_intro_username_rules(tr::now);
		break;
	case HandleState::Short:
		text = tr::lng_intro_username_needs_at_least_3(tr::now);
		break;
	case HandleState::Checking:
		text = tr::lng_intro_checking_username(tr::now);
		break;
	case HandleState::Unknown:
		text = tr::lng_profile_edit_handle_unknown(tr::now);
		break;
	case HandleState::Taken:
		text = tr::lng_intro_username_taken(tr::now);
		break;
	case HandleState::Free:
		text = tr::lng_intro_username_available(tr::now);
		break;
	}
	setHint(_handleHint, text, HandleIsError(state), state == HandleState::Free);
	if (HandleIsError(state)) {
		_handle->showError();
	} else {
		_handle->hideError();
	}
}

void EditSection::refreshLinkHint() {
	const auto ok = IsValidLink(_link->text());
	setHint(
		_linkHint,
		ok ? QString() : tr::lng_profile_edit_link_invalid(tr::now),
		!ok);
	if (ok) {
		_link->hideError();
	} else {
		_link->showError();
	}
}

void EditSection::refreshBioCounter() {
	const auto length = int(_bio->text().size());
	setHint(
		_bioCounter,
		tr::lng_profile_edit_counter(
			tr::now,
			lt_n,
			Lang::Num(length),
			lt_max,
			Lang::Num(_bioMax)),
		length > _bioMax);
	if (length > _bioMax) {
		_bio->showError();
	} else {
		_bio->hideError();
	}
}

void EditSection::refreshBirthday() {
	_birthdayRow->setText(_birthday
		? BirthdayText(*_birthday)
		: tr::lng_profile_edit_birthday_none(tr::now));
	setHint(
		_birthdayHint,
		_birthdayError.isEmpty()
			? tr::lng_profile_edit_birthday_hint(tr::now)
			: _birthdayError,
		!_birthdayError.isEmpty());
}

void EditSection::refreshPhoto() {
	const auto text = _uploading
		? tr::lng_profile_edit_photo_uploading(tr::now)
		: (_clearPhoto && !_base.avatarId.isEmpty())
		? tr::lng_profile_edit_photo_removed_hint(tr::now)
		: QString();
	setHint(_photoHint, text, false);
}

bool EditSection::dirty() const {
	if (!_filled) {
		return false;
	}
	return _hasNewPhoto
		|| (_clearPhoto && !_base.avatarId.isEmpty())
		|| (_name->text().trimmed() != _base.displayName)
		|| (NormalizeHandle(_handle->text()) != _base.handle)
		|| (NormalizeLink(_link->text()) != NormalizeLink(_base.link))
		|| (_bio->text() != _base.bio)
		|| !SameBirthday(_birthday, _base.birthday);
}

bool EditSection::valid() const {
	const auto name = _name->text().trimmed();
	return _filled
		&& !name.isEmpty()
		&& (name.size() <= _nameMax)
		&& (_bio->text().size() <= _bioMax)
		&& IsValidLink(_link->text())
		&& !HandleBlocksSave(_checker.state())
		&& !NormalizeHandle(_handle->text()).isEmpty();
}

void EditSection::refreshState() {
	if (!_save) {
		return;
	}
	_save->setLoading(_saving || _saveAfterUpload);
	_save->setDisabled(!(_saving || _saveAfterUpload) && !(dirty() && valid()));
	_cancel->setDisabled(_saving);
}

void EditSection::showPhotoMenu() {
	if (!_filled || _saving) {
		return;
	}
	const auto has = _hasNewPhoto
		|| (!_base.avatarId.isEmpty() && !_clearPhoto);
	if (!has) {
		choosePhoto();
		return;
	}
	if (_menu) {
		_menu->hideMenu(true);
	}
	_menu = new Ui::PopupMenu(this);
	_menu->addAction(
		tr::lng_profile_edit_choose_photo(tr::now),
		[this] { choosePhoto(); },
		&st::profileMenuPhotoIcon);
	auto item = Ui::MenuItem();
	item.text = tr::lng_profile_edit_remove_photo(tr::now);
	item.icon = &st::profileMenuRemoveIcon;
	item.destructive = true;
	item.callback = [this] { removePhoto(); };
	_menu->addItem(std::move(item));
	_menu->popupAnchored(_changePhoto, style::al_center);
}

void EditSection::choosePhoto() {
	const auto controller = this->controller();
	const auto weak = QPointer<EditSection>(this);
	Boxes::PickAndCropPhoto(this, [=](Ui::object_ptr<Ui::BoxContent> box) {
		controller->show(std::move(box));
	}, [=](const QImage &square) {
		if (weak) {
			weak->startUpload(square);
		}
	}, [=] {
		controller->showToast(tr::lng_intro_photo_not_picture(tr::now));
	});
}

void EditSection::cropPhoto(const QImage &image) {
	if (image.isNull() || image.width() < 8 || image.height() < 8) {
		controller()->showToast(tr::lng_intro_photo_not_picture(tr::now));
		return;
	}
	const auto weak = QPointer<EditSection>(this);
	controller()->show(Ui::Box<Boxes::PhotoCropBox>(image, [=](
			const QImage &square) {
		if (weak) {
			weak->startUpload(square);
		}
	}));
}

void EditSection::cropPhotoFile(const QString &path) {
	const auto weak = QPointer<EditSection>(this);
	// Decoding a photo may take a while: off the GUI thread.
	QThreadPool::globalInstance()->start([=] {
		auto image = Data::DecodeImageFile(path, kMaxDroppedSide);
		if (!image.isNull() && image.hasAlphaChannel()) {
			auto flat = QImage(image.size(), QImage::Format_RGB32);
			flat.fill(Qt::white);
			auto p = QPainter(&flat);
			p.drawImage(0, 0, image);
			p.end();
			image = flat;
		}
		QMetaObject::invokeMethod(qApp, [=] {
			if (weak) {
				weak->cropPhoto(image);
			}
		}, Qt::QueuedConnection);
	});
}

void EditSection::dragEnterEvent(QDragEnterEvent *e) {
	if (!_filled || _saving || !Support::CanAcceptDrop(e->mimeData())) {
		e->ignore();
		return;
	}
	const auto data = Support::ReadMimeData(e->mimeData());
	const auto picture = !data.image.isNull()
		|| (data.files.size() == 1 && Support::IsImageFile(data.files.front()));
	if (picture) {
		e->setDropAction(Qt::CopyAction);
		e->accept();
	} else {
		e->ignore();
	}
}

void EditSection::dropEvent(QDropEvent *e) {
	if (!_filled || _saving) {
		e->ignore();
		return;
	}
	const auto data = Support::ReadMimeData(e->mimeData());
	if (!data.image.isNull()) {
		cropPhoto(data.image);
	} else if (data.files.size() == 1
		&& Support::IsImageFile(data.files.front())) {
		cropPhotoFile(data.files.front());
	} else {
		e->ignore();
		return;
	}
	e->setDropAction(Qt::CopyAction);
	e->accept();
}

void EditSection::startUpload(const QImage &square) {
	cancelUpload();
	_hasNewPhoto = true;
	_clearPhoto = false;
	_uploading = true;
	_uploadedMediaId = QString();
	const auto generation = ++_photoGeneration;
	_photo->setCleared(false);
	_photo->setImage(square);
	_photo->setUploading(true);
	refreshPhoto();
	refreshState();
	// The upload runs in the background while the rest is being edited.
	Boxes::SaveCroppedPhoto(square, this, [=, this](const QString &path) {
		if (generation != _photoGeneration) {
			return;
		}
		const auto failed = [=, this] {
			if (generation != _photoGeneration) {
				return;
			}
			_uploadId = 0;
			_uploading = false;
			_hasNewPhoto = false;
			_photo->setUploading(false);
			_photo->setImage(QImage());
			refreshPhoto();
			setHint(_photoHint, tr::lng_chat_upload_failed(tr::now), true);
			_saveAfterUpload = false;
			refreshState();
		};
		if (path.isEmpty()) {
			failed();
			return;
		}
		auto request = Data::UploadRequest();
		request.filePath = path;
		request.kind = Data::UploadKind::Image;
		request.purpose = Data::UploadPurpose::Avatar;
		auto callbacks = Data::UploadCallbacks();
		callbacks.progress = [=, this](qint64 sent, qint64 total) {
			if (generation == _photoGeneration && total > 0) {
				_photo->setUploading(true, sent / double(total));
			}
		};
		callbacks.done = [=, this](const Data::UploadResult &result) {
			if (generation != _photoGeneration) {
				return;
			}
			_uploadId = 0;
			_uploading = false;
			_uploadedMediaId = result.mediaId;
			_photo->setUploading(false);
			refreshPhoto();
			if (_saveAfterUpload) {
				_saveAfterUpload = false;
				save();
			} else {
				refreshState();
			}
		};
		callbacks.fail = [=](const Api::Error &) { failed(); };
		_uploadId = this->controller()->session().data().upload().start(
			request,
			this,
			std::move(callbacks));
	});
}

void EditSection::cancelUpload() {
	++_photoGeneration;
	if (_uploadId) {
		controller()->session().data().upload().cancel(_uploadId);
		_uploadId = 0;
	}
	_uploading = false;
}

void EditSection::removePhoto() {
	cancelUpload();
	_hasNewPhoto = false;
	_uploadedMediaId = QString();
	_clearPhoto = true;
	_saveAfterUpload = false;
	_photo->setUploading(false);
	_photo->setImage(QImage());
	_photo->setCleared(true);
	refreshPhoto();
	refreshState();
}

void EditSection::save() {
	if (_saving || !dirty() || !valid()) {
		return;
	}
	if (_uploading) {
		// The photo is still on its way: saving continues when it arrives.
		_saveAfterUpload = true;
		refreshState();
		return;
	}
	_saving = true;
	refreshState();
	saveProfile();
}

void EditSection::saveProfile() {
	auto patch = Data::ProfilePatch();
	auto changed = false;
	if (_hasNewPhoto && !_uploadedMediaId.isEmpty()) {
		patch.avatarId = _uploadedMediaId;
		changed = true;
	} else if (_clearPhoto && !_base.avatarId.isEmpty()) {
		patch.clearAvatar = true;
		changed = true;
	}
	const auto name = _name->text().trimmed();
	if (name != _base.displayName) {
		patch.displayName = name;
		changed = true;
	}
	if (_bio->text() != _base.bio) {
		patch.bio = _bio->text();
		changed = true;
	}
	const auto link = NormalizeLink(_link->text());
	if (link != NormalizeLink(_base.link)) {
		patch.link = link;
		changed = true;
	}
	if (!changed) {
		saveHandle();
		return;
	}
	controller()->session().data().graph().updateProfile(
		patch,
		this,
		[this](const Data::Profile &profile) {
			// What was saved is the new base: a later failure of the handle
			// or the birthday leaves only those two dirty.
			_base.displayName = profile.displayName;
			_base.bio = profile.bio;
			_base.link = profile.link;
			_base.avatarId = profile.avatarId;
			_hasNewPhoto = false;
			_clearPhoto = false;
			_uploadedMediaId = QString();
			_photo->setUser(profile);
			_photo->setImage(QImage());
			_photo->setCleared(false);
			refreshPhoto();
			saveHandle();
		},
		[this](const Api::Error &error) { saveFailed(error, Field::None); });
}

void EditSection::saveHandle() {
	const auto handle = NormalizeHandle(_handle->text());
	if (handle == _base.handle) {
		saveBirthday();
		return;
	}
	controller()->session().data().graph().setHandle(
		handle,
		this,
		[this](const Data::Profile &profile) {
			_base.handle = profile.handle;
			_checker.setCurrent(profile.handle);
			saveBirthday();
		},
		[this](const Api::Error &error) { saveFailed(error, Field::Handle); });
}

void EditSection::saveBirthday() {
	if (SameBirthday(_birthday, _base.birthday)) {
		finishSave();
		return;
	}
	auto &graph = controller()->session().data().graph();
	const auto fail = [this](const Api::Error &error) {
		saveFailed(error, Field::Birthday);
	};
	if (_birthday) {
		graph.setBirthday(*_birthday, this, [this](const Data::Profile &) {
			_base.birthday = _birthday;
			finishSave();
		}, fail);
	} else {
		graph.clearBirthday(this, [this] {
			_base.birthday = std::nullopt;
			finishSave();
		}, fail);
	}
}

void EditSection::finishSave() {
	const auto controller = this->controller();
	// The own profile and every card of mine follow `meChanged`.
	controller->session().data().users().refreshMe();
	controller->showToast(tr::lng_profile_profile_updated(tr::now));
	_saving = false;
	leave();
}

EditSection::Field EditSection::FieldOf(const QString &code, Field fallback) {
	if (code.startsWith(u"handle.")) {
		return Field::Handle;
	} else if (code.startsWith(u"birthday.")) {
		return Field::Birthday;
	} else if (code == u"profile.link_invalid") {
		return Field::Link;
	} else if (code == u"profile.badge_character") {
		return Field::Name;
	} else if (code == u"profile.too_long") {
		return Field::Bio;
	} else if (code == u"profile.avatar_invalid"
		|| code.startsWith(u"media.")
		|| code.startsWith(u"upload.")) {
		return Field::Photo;
	}
	return fallback;
}

void EditSection::saveFailed(const Api::Error &error, Field fallback) {
	_saving = false;
	const auto text = Support::ErrorText(
		error,
		tr::lng_profile_couldnt_save(tr::now));
	auto target = static_cast<Ui::RpWidget*>(nullptr);
	switch (FieldOf(error.code, error.offline() ? Field::None : fallback)) {
	case Field::Handle:
		if (error.code == u"handle.taken") {
			_checker.markTaken();
		} else {
			_handleError = text;
		}
		refreshHandleHint();
		_handle->showError();
		target = _handle;
		break;
	case Field::Birthday:
		_birthdayError = text;
		refreshBirthday();
		target = _birthdayRow;
		break;
	case Field::Link:
		setHint(_linkHint, text, true);
		_link->showError();
		target = _link;
		break;
	case Field::Name:
		setHint(_nameHint, text, true);
		_name->showError();
		target = _name;
		break;
	case Field::Bio:
		setHint(_bioCounter, text, true);
		_bio->showError();
		target = _bio;
		break;
	case Field::Photo:
		setHint(_photoHint, text, true);
		target = _photo;
		break;
	case Field::None:
		Support::ShowError(
			controller(),
			error,
			tr::lng_profile_couldnt_save(tr::now));
		break;
	}
	if (target) {
		_scroll->scrollToWidget(target, true);
	}
	refreshState();
}

bool EditSection::onBackRequest() {
	if (_leaving || !dirty()) {
		return false;
	} else if (_saving) {
		return true; // the answer of the server decides
	}
	const auto weak = QPointer<EditSection>(this);
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_profile_edit_discard_title(tr::now);
	args.text = tr::lng_profile_edit_discard_text(tr::now);
	args.confirmText = tr::lng_profile_edit_discard(tr::now);
	args.cancelText = tr::lng_profile_edit_keep(tr::now);
	args.destructive = true;
	args.confirmed = [=](std::function<void()> close) {
		close();
		if (weak) {
			weak->leave();
		}
	};
	controller()->show(Ui::MakeConfirmBox(std::move(args)));
	return true;
}

void EditSection::leave() {
	_leaving = true;
	const auto controller = this->controller();
	// Destroys this section: deferred, and nothing follows.
	Ui::PostponeCall(controller, [=] { controller->showBack(); });
}

void EditSection::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	const auto footer = int(st::profileEditFooter);
	const auto top = _bar->height();
	_scroll->setGeometry(
		0,
		top,
		width(),
		std::max(height() - top - footer, 0));
	const auto column = CenteredColumn(width(), st::profileEditWidth);
	const auto padding = int(st::profileEditFieldMargin.left());
	const auto buttonTop = height() - footer + (footer - _save->height()) / 2;
	_save->resizeToNaturalWidth(column.width);
	_cancel->resizeToNaturalWidth(column.width);
	// The main action sits on the trailing side, like in boxes.
	_save->moveToRight(
		width() - column.left - column.width + padding,
		buttonTop,
		width());
	_cancel->moveToRight(
		width() - column.left - column.width + padding + _save->width() + st::spaceSm,
		buttonTop,
		width());
}

void EditSection::paintEvent(QPaintEvent *e) {
	SectionWidget::paintEvent(e);
	auto p = QPainter(this);
	p.fillRect(
		0,
		height() - st::profileEditFooter,
		width(),
		st::lineWidth,
		st::chromeHairline);
}

void EditSection::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_S && (e->modifiers() & Qt::ControlModifier)) {
		save();
		return;
	}
	SectionWidget::keyPressEvent(e);
}

} // namespace Profile
