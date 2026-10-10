#include "intro/intro_steps.h"

#include "api/api_client_config.h"
#include "boxes/country_select_box.h"
#include "boxes/photo_crop_box.h"
#include "core/core_application.h"
#include "data/data_session.h"
#include "data/data_upload.h"
#include "data/data_users.h"
#include "lang/lang_countries.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "support/brand_mark.h"
#include "support/error_text.h"
#include "support/links.h"
#include "ui/effects/radial_animation.h"
#include "ui/image/userpic.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "styles/style_intro.h"

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>

namespace Intro {
namespace {

constexpr auto kHandleCheckDelay = 450;
const auto kTermsUrl = QStringLiteral("https://eleza.me/terms");
const auto kPrivacyUrl = QStringLiteral("https://eleza.me/privacy");

[[nodiscard]] qint64 NowMs() {
	return QDateTime::currentMSecsSinceEpoch();
}

// A phone number / e-mail inside a sentence of the other direction.
[[nodiscard]] QString Isolated(const QString &text) {
	return Lang::Ltr(text);
}

[[nodiscard]] const Lang::Country *DefaultCountry() {
	return Lang::CountryByIso(Lang::IsPersian()
		? QStringLiteral("IR")
		: QStringLiteral("US"));
}

// A fixed-height row whose children are placed by the owner.
class Slot final : public Ui::RpWidget {
public:
	Slot(QWidget *parent, int height) : RpWidget(parent) {
		resize(width(), height);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return height();
	}

};

class MarkWidget final : public Ui::RpWidget {
public:
	explicit MarkWidget(QWidget *parent) : RpWidget(parent) {
		resize(st::introMarkSize);
	}

	int naturalWidth() const override {
		return st::introMarkSize.width();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return st::introMarkSize.height();
	}
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = QPainter(this);
		Support::PaintBrandMark(p, rect());
	}

};

// "🇮🇷 ایران ⌄": the row that opens the country list.
class CountryButton final : public Ui::RippleButton {
public:
	explicit CountryButton(QWidget *parent)
	: RippleButton(parent, st::defaultRipple)
	, _flags(Lang::FlagEmojiSupported()) {
		resize(width(), st::introCountryHeight);
	}

	void setCountry(const Lang::Country *country) {
		_iso = country ? country->iso : QString();
		_name = country ? country->name() : tr::lng_intro_country(tr::now);
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return st::introCountryHeight;
	}
	QImage prepareRippleMask() const override {
		return Ui::RippleAnimation::RoundRectMask(
			size(),
			st::introInputField.radius);
	}
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		Ui::PaintInputFrame(p, rect(), st::introInputField, 0., 0.);
		paintRipple(p, 0, 0);

		const auto &padding = st::introCountryPadding;
		auto left = padding.left();
		if (!_iso.isEmpty()) {
			const auto flag = myrtlrect(left, 0, st::introCountryFlagWidth, height());
			if (_flags) {
				p.setFont(st::introCountryFlagFont);
				p.setPen(st::textPrimary);
				p.drawText(flag, Qt::AlignCenter, Lang::FlagEmoji(_iso));
			} else {
				p.setFont(st::introCountryIsoFont);
				p.setPen(st::textSecondary);
				p.drawText(flag, Qt::AlignCenter, _iso);
			}
			left += st::introCountryFlagWidth + st::introCountrySkip;
		}
		const auto &chevron = st::introCountryChevron;
		const auto chevronLeft = width() - padding.right() - chevron.width();
		chevron.paint(p, chevronLeft, (height() - chevron.height()) / 2, width());
		const auto available = chevronLeft - st::introCountrySkip - left;
		p.setFont(st::introCountryNameFont);
		p.setPen(st::textPrimary);
		p.drawTextLeft(
			left,
			(height() - st::introCountryNameFont->height) / 2,
			width(),
			st::introCountryNameFont->elided(_name, available));
	}

private:
	QString _iso;
	QString _name;
	bool _flags = false;

};

// «ارسال دوباره تا ۰:۴۵», then the «ارسال دوباره» link.
class ResendRow final : public Ui::RpWidget {
public:
	ResendRow(
		QWidget *parent,
		std::function<qint64()> resendAt,
		std::function<void()> resend)
	: RpWidget(parent)
	, _resendAt(std::move(resendAt))
	, _link(new Ui::LinkButton(this, tr::lng_intro_resend(tr::now), st::introLink)) {
		resize(width(), st::introResendHeight);
		_link->setClickedCallback(std::move(resend));
		_timer.setInterval(500);
		QObject::connect(&_timer, &QTimer::timeout, this, [this] { refresh(); });
		_timer.start();
		refresh();
	}

	void refresh() {
		const auto left = (_resendAt() - NowMs() + 999) / 1000;
		const auto waiting = (left > 0);
		const auto text = waiting
			? tr::lng_intro_resend_in(
				tr::now,
				lt_time,
				Lang::Duration(int(left)))
			: QString();
		_link->setVisible(!waiting && !_busy);
		if (_text != text) {
			_text = text;
			update();
		}
	}
	// A resend request is on its way.
	void setBusy(bool busy) {
		_busy = busy;
		refresh();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		_link->resizeToNaturalWidth(newWidth);
		_link->move(
			(newWidth - _link->width()) / 2,
			(st::introResendHeight - _link->height()) / 2);
		return st::introResendHeight;
	}
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		if (_text.isEmpty()) {
			return;
		}
		auto p = Painter(this);
		p.setFont(st::introHintStyle.font);
		p.setPen(st::introHintFg);
		p.drawTextCentered(rect(), _text);
	}

private:
	std::function<qint64()> _resendAt;
	Ui::LinkButton *_link = nullptr;
	QTimer _timer;
	QString _text;
	bool _busy = false;

};

// The part of HandleField that edits: lowercase Latin letters, digits, "." and "_".
class HandleInput final : public Ui::MaskedInputField {
public:
	using MaskedInputField::MaskedInputField;

protected:
	void correctValue(
			const QString &was,
			int wasCursor,
			QString &now,
			int &nowCursor) override {
		Q_UNUSED(was);
		Q_UNUSED(wasCursor);
		auto result = QString();
		auto cursor = nowCursor;
		const auto lower = now.toLower();
		for (auto i = 0; i != lower.size(); ++i) {
			const auto ch = lower[i].unicode();
			const auto allowed = (ch >= 'a' && ch <= 'z')
				|| (ch >= '0' && ch <= '9')
				|| (ch == '_')
				|| (ch == '.');
			// A pasted "@name" loses its sign silently.
			if (allowed && result.size() < 24) {
				result.append(lower[i]);
			} else if (i < nowCursor) {
				--cursor;
			}
		}
		now = result;
		nowCursor = std::clamp(cursor, 0, int(result.size()));
	}

};

// "@ handle" in one frame, always left-to-right.
class HandleField final : public Ui::RpWidget {
public:
	explicit HandleField(QWidget *parent)
	: RpWidget(parent)
	, _input(new HandleInput(
		this,
		st::introPartField,
		tr::lng_username(tr::now))) {
		_input->setForcedDirection(Qt::LeftToRight);
		setCursor(style::cur_text);
		setFocusProxy(_input);
		resize(width(), st::introInputField.heightMin);
		QObject::connect(
			_input,
			&Ui::MaskedInputField::focusedChanged,
			this,
			[this](bool focused) {
				_focusAnimation.start(
					[this] { update(); },
					focused ? 0. : 1.,
					focused ? 1. : 0.,
					st::introInputField.duration);
				_focused = focused;
				update();
			});
		QObject::connect(_input, &Ui::MaskedInputField::changed, this, [this] {
			if (_error) {
				_error = false;
				update();
			}
		});
	}

	[[nodiscard]] HandleInput *input() const {
		return _input;
	}
	[[nodiscard]] QString text() const {
		return _input->text();
	}
	void showError() {
		_error = true;
		update();
		_input->setFocusFast();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto height = int(st::introInputField.heightMin);
		const auto &margins = st::introInputField.textMargins;
		const auto left = margins.left() + st::introHandlePrefixWidth;
		_input->resizeToWidth(std::max(newWidth - left - margins.right(), 1));
		_input->move(left, (height - _input->height()) / 2);
		return height;
	}
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		Ui::PaintInputFrame(
			p,
			rect(),
			st::introInputField,
			_focusAnimation.value(_focused ? 1. : 0.),
			_error ? 1. : 0.);
		p.setFont(st::introHandlePrefixFont);
		p.setPen(st::introHandlePrefixFg);
		p.drawText(
			QRect(
				st::introInputField.textMargins.left(),
				0,
				st::introHandlePrefixWidth,
				height()),
			Qt::AlignLeft | Qt::AlignVCenter,
			QStringLiteral("@"));
	}
	void mousePressEvent(QMouseEvent *e) override {
		Q_UNUSED(e);
		_input->setFocusFast();
	}

private:
	HandleInput *_input = nullptr;
	bool _focused = false;
	bool _error = false;
	Ui::Animations::Simple _focusAnimation;

};

// The round photo of the profile step: a camera until a picture is chosen,
// then the picture with the upload progress over it.
class PhotoButton final : public Ui::AbstractButton {
public:
	explicit PhotoButton(QWidget *parent)
	: AbstractButton(parent)
	, _progress([this] { update(); }) {
		resize(st::introPhotoSize, st::introPhotoSize);
	}

	void setImage(const QImage &image) {
		_image = image;
		_prepared = QImage();
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
		return st::introPhotoSize;
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return st::introPhotoSize;
	}
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto size = int(st::introPhotoSize);
		const auto circle = QRect((width() - size) / 2, 0, size, size);
		if (_image.isNull()) {
			p.setPen(QPen(st::introPhotoBorder->c, 1.));
			p.setBrush(st::introPhotoBg);
			p.drawEllipse(QRectF(circle).adjusted(0.5, 0.5, -0.5, -0.5));
			st::introPhotoIcon.paintInCenter(p, circle);
			return;
		}
		const auto ratio = devicePixelRatioF();
		if (_prepared.isNull() || _preparedRatio != ratio) {
			_prepared = Ui::PrepareUserpic(_image, size, ratio);
			_preparedRatio = ratio;
		}
		p.drawImage(circle, _prepared);
		if (_uploading) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::introPhotoProgressBg);
			p.drawEllipse(circle);
			const auto ring = circle.adjusted(28, 28, -28, -28);
			_progress.draw(p, ring, 3., st::introPhotoProgressFg->c);
			return;
		}
		// A small camera badge: the photo can be changed.
		const auto badge = int(st::introPhotoBadgeSize);
		const auto badgeRect = style::rtlrect(
			circle.x() + size - badge,
			size - badge,
			badge,
			badge,
			width());
		p.setPen(QPen(st::introBg->c, 2.));
		p.setBrush(st::introPhotoBadgeBg);
		p.drawEllipse(badgeRect);
		st::introPhotoBadgeIcon.paintInCenter(p, badgeRect);
	}

private:
	QImage _image;
	QImage _prepared;
	double _preparedRatio = 0.;
	bool _uploading = false;
	Ui::RadialProgressAnimation _progress;

};

// ---- welcome

class WelcomeStep final : public Step {
public:
	WelcomeStep(QWidget *parent, Widget *intro) : Step(parent, intro) {
		const auto layout = content();
		layout->add(
			Ui::object_ptr<MarkWidget>(layout),
			style::margins(0, 0, 0, st::introMarkSkip),
			style::al_top);
		layout->add(Ui::object_ptr<Ui::FlatLabel>(
			layout,
			tr::lng_app_name(tr::now),
			st::introWordmark));
		layout->add(
			Ui::object_ptr<Ui::FlatLabel>(
				layout,
				tr::lng_intro_tagline(tr::now),
				st::introTagline),
			style::margins(0, 0, 0, st::introTaglineSkip));
		_start = layout->add(
			Ui::object_ptr<Ui::RoundButton>(
				layout,
				tr::lng_intro_start(tr::now),
				st::introStartButton),
			style::margins(),
			style::al_top);
		_start->setFullWidth(st::introStartWidth);
		_start->setClickedCallback([this] { submit(); });
		const auto other = layout->add(
			Ui::object_ptr<Ui::LinkButton>(
				layout,
				tr::lng_intro_switch_language(tr::now),
				st::introSwitchLink),
			style::margins(0, st::introSwitchSkip, 0, 0),
			style::al_top);
		other->setClickedCallback([] {
			// The intro is rebuilt in the other language.
			QTimer::singleShot(0, &Core::App(), [] {
				Core::App().setLanguage(Lang::IsPersian()
					? Lang::Language::English
					: Lang::Language::Persian);
			});
		});

		// «با ادامه، قوانین و سیاست حریم خصوصی الزا را می‌پذیری.» with both
		// documents as links.
		auto legal = Ui::Text::TextWithEntities();
		legal.text = tr::lng_intro_terms(tr::now);
		const auto link = [&](const QString &phrase, const QString &url) {
			const auto offset = legal.text.indexOf(phrase);
			if (offset >= 0 && !phrase.isEmpty()) {
				auto entity = Ui::Text::Entity();
				entity.type = Ui::Text::EntityType::CustomUrl;
				entity.offset = int(offset);
				entity.length = int(phrase.size());
				entity.data = url;
				legal.entities.push_back(entity);
			}
		};
		link(tr::lng_intro_terms_link(tr::now), kTermsUrl);
		link(tr::lng_intro_privacy_link(tr::now), kPrivacyUrl);
		const auto label = layout->add(
			Ui::object_ptr<Ui::FlatLabel>(layout, st::introLegal),
			style::margins(0, st::introLegalSkip, 0, 0));
		label->setMarkedText(legal);
		label->setLinkHandler([](const Ui::Text::Link &link, Qt::MouseButton) {
			Support::OpenLinkExternally(Ui::Text::LinkUrl(link));
			return true;
		});
	}

	bool hasBack() const override {
		return false;
	}
	bool night() const override {
		return true;
	}
	void submit() override {
		intro()->showStep(StepType::Phone);
	}

private:
	Ui::RoundButton *_start = nullptr;

};

// ---- phone

class PhoneStep final : public Step {
public:
	PhoneStep(QWidget *parent, Widget *intro) : Step(parent, intro) {
		auto &data = state();
		if (data.countryIso.isEmpty()) {
			if (const auto country = DefaultCountry()) {
				data.countryIso = country->iso;
				data.countryCode = country->code;
			}
		}
		addTitle(
			tr::lng_intro_phone_title(tr::now),
			tr::lng_intro_phone_about(tr::now));
		const auto layout = content();
		_country = layout->add(
			Ui::object_ptr<CountryButton>(layout),
			style::margins(0, 0, 0, st::introFieldSkip));
		_country->setCountry(Lang::CountryByIso(data.countryIso));
		_country->setClickedCallback([this] { chooseCountry(); });

		_phone = layout->add(Ui::object_ptr<Ui::PhoneInput>(
			layout,
			st::introInputField,
			data.countryCode));
		_phone->setNumberPlaceholder(tr::lng_intro_phone_placeholder(tr::now));
		_phone->setNumber(data.number);

		_error = layout->add(
			Ui::object_ptr<HintLine>(layout),
			style::margins(0, st::introHintSkip, 0, 0));

		// Asked in the English interface only.
		if (!Lang::IsPersian()) {
			_adult = layout->add(
				Ui::object_ptr<Ui::Checkbox>(
					layout,
					tr::lng_intro_adult_confirm(tr::now),
					data.adultConfirmed),
				style::margins(0, 0, 0, st::introFieldSkip));
			QObject::connect(
				_adult,
				&Ui::Checkbox::checkedChanged,
				this,
				[this](bool checked) {
					state().adultConfirmed = checked;
					refreshButton();
				});
		}

		_submit = layout->add(
			Ui::object_ptr<Ui::RoundButton>(
				layout,
				tr::lng_intro_get_code(tr::now),
				st::introButton),
			style::margins(0, st::introButtonSkip, 0, 0));
		_submit->setClickedCallback([this] { submit(); });

		QObject::connect(_phone, &Ui::PhoneInput::changed, this, [this] {
			state().number = _phone->number();
			_error->clear();
			refreshButton();
		});
		QObject::connect(
			_phone,
			&Ui::PhoneInput::countryCodeChanged,
			this,
			[this](const QString &code) {
				auto &data = state();
				data.countryCode = code;
				// Keep the chosen country when it shares the code (+1, +7).
				const auto current = Lang::CountryByIso(data.countryIso);
				if (!current || current->code != code) {
					const auto found = Lang::CountryByCode(code);
					data.countryIso = found ? found->iso : QString();
					_country->setCountry(found);
				}
			});
		QObject::connect(
			_phone,
			&Ui::PhoneInput::internationalNumberEntered,
			this,
			[this](const QString &digits) {
				if (const auto country = Lang::CountryByPhone(digits)) {
					state().countryIso = country->iso;
					_country->setCountry(country);
					_phone->setCountryCode(country->code);
					_phone->setNumber(digits.mid(country->code.size()));
				}
			});
		QObject::connect(_phone, &Ui::PhoneInput::submitted, this, [this] {
			submit();
		});
		QObject::connect(_phone, &Ui::PhoneInput::cancelled, this, [this] {
			goBack();
		});
		refreshButton();
	}

	void setInnerFocus() override {
		_phone->setFocusFast();
	}
	void goBack() override {
		intro()->showStep(StepType::Welcome, true);
	}
	void submit() override {
		const auto phone = fullNumber();
		if (_working) {
			return;
		} else if (phone.isEmpty()) {
			_phone->showError();
			_error->setError(tr::lng_intro_invalid_phone(tr::now));
			return;
		} else if (_adult && !_adult->checked()) {
			return;
		}
		setWorking(true);
		intro()->account().auth().requestCode(phone, this, [=, this](
				const Api::OtpChallenge &challenge) {
			auto &data = state();
			data.phone = phone;
			data.challenge = challenge;
			data.resendAt = NowMs() + qint64(challenge.resendAfter) * 1000;
			intro()->showStep(StepType::Code);
		}, [this](const Api::Error &error) {
			setWorking(false);
			_error->setError(Support::ErrorText(error));
			if (error.status == 400 || error.status == 422) {
				_phone->showError();
			}
		});
	}

private:
	[[nodiscard]] QString fullNumber() const {
		const auto code = _phone->countryCode();
		return code.isEmpty()
			? QString()
			: Lang::E164(_phone->number(), code);
	}
	void refreshButton() {
		_submit->setDisabled(fullNumber().isEmpty()
			|| (_adult && !_adult->checked()));
	}
	void setWorking(bool working) {
		_working = working;
		_submit->setLoading(working);
	}
	void chooseCountry() {
		const auto weak = QPointer<PhoneStep>(this);
		ShowCountryBox(intro(), [this, weak](const QString &iso) {
			if (!weak) {
				return; // the step was left while the list was open
			} else if (const auto country = Lang::CountryByIso(iso)) {
				state().countryIso = country->iso;
				_country->setCountry(country);
				_phone->setCountryCode(country->code);
				_phone->setFocusFast();
			}
		});
	}

	CountryButton *_country = nullptr;
	Ui::PhoneInput *_phone = nullptr;
	HintLine *_error = nullptr;
	Ui::Checkbox *_adult = nullptr;
	Ui::RoundButton *_submit = nullptr;
	bool _working = false;

};

// A code as boxes, the error line, the resend row and a slot with either a
// spinner (checking) or «تلاش دوباره». Shared by the SMS and the e-mail codes.
class CodeStepBase : public Step {
public:
	using Step::Step;

	void setInnerFocus() override {
		_code->setFocusFast();
	}
	void submit() override {
		const auto code = _code->code();
		if (!_working && code.size() == _length) {
			verify(code);
		}
	}

protected:
	void addCodeRows(
			int length,
			std::function<qint64()> resendAt,
			std::function<void()> resend) {
		_length = std::clamp(length, 4, 8);
		const auto layout = content();
		_code = layout->add(
			Ui::object_ptr<Ui::CodeInput>(layout, _length, st::introCodeInput),
			style::margins(0, st::introCodeSkip, 0, 0));
		_error = layout->add(
			Ui::object_ptr<HintLine>(layout),
			style::margins(0, st::introHintSkip + 4, 0, 0));
		_resend = layout->add(Ui::object_ptr<ResendRow>(
			layout,
			std::move(resendAt),
			std::move(resend)));
		const auto slot = layout->add(
			Ui::object_ptr<Slot>(layout, st::introButton.height),
			style::margins(0, st::introButtonSkip, 0, 0));
		_spinner = new Ui::LoadingSpinner(
			slot,
			st::introSpinnerSize,
			st::brandPrimary);
		_retry = new Ui::RoundButton(
			slot,
			tr::lng_try_again(tr::now),
			st::introButton);
		_spinner->hide();
		_retry->hide();
		_retry->setClickedCallback([this] { submit(); });
		QObject::connect(slot, &Ui::RpWidget::widthChanged, this, [=, this](
				int width) {
			_retry->resizeToWidth(width);
			_retry->move(0, 0);
			_spinner->move(
				(width - _spinner->width()) / 2,
				(slot->height() - _spinner->height()) / 2);
		});

		QObject::connect(_code, &Ui::CodeInput::codeChanged, this, [this] {
			_error->clear();
			_retry->hide();
		});
		QObject::connect(_code, &Ui::CodeInput::finished, this, [this](
				const QString &code) {
			if (!_working) {
				verify(code);
			}
		});
		QObject::connect(_code, &Ui::CodeInput::cancelled, this, [this] {
			goBack();
		});
	}

	virtual void verify(const QString &code) = 0;

	void setWorking(bool working) {
		_working = working;
		_code->setDisabled(working);
		_spinner->setVisible(working);
		if (working) {
			_retry->hide();
			_error->clear();
		}
	}
	// A wrong code: red boxes, a shake, the message and «تلاش دوباره».
	void showCodeError(const QString &text, bool wrongCode) {
		setWorking(false);
		_error->setError(text);
		if (wrongCode) {
			_code->showError();
		}
		_retry->setVisible(_code->code().size() == _length);
		_code->setFocusFast();
	}
	void showError(const QString &text) {
		_error->setError(text);
	}
	[[nodiscard]] ResendRow *resendRow() const {
		return _resend;
	}
	[[nodiscard]] bool working() const {
		return _working;
	}

private:
	int _length = 6;
	Ui::CodeInput *_code = nullptr;
	HintLine *_error = nullptr;
	ResendRow *_resend = nullptr;
	Ui::LoadingSpinner *_spinner = nullptr;
	Ui::RoundButton *_retry = nullptr;
	bool _working = false;

};

// ---- the SMS code

class CodeStep final : public CodeStepBase {
public:
	CodeStep(QWidget *parent, Widget *intro) : CodeStepBase(parent, intro) {
		addTitle(tr::lng_intro_code_title(tr::now));
		const auto layout = content();
		layout->add(Ui::object_ptr<Ui::FlatLabel>(
			layout,
			tr::lng_intro_code_sent_to(
				tr::now,
				lt_phone,
				Lang::Phone(state().phone)),
			st::introAbout));
		const auto edit = layout->add(
			Ui::object_ptr<Ui::LinkButton>(
				layout,
				tr::lng_edit(tr::now),
				st::introLink),
			style::margins(0, st::introSentSkip, 0, 0),
			style::al_top);
		edit->setClickedCallback([this] { goBack(); });
		addCodeRows(
			state().challenge.codeLength,
			[this] { return state().resendAt; },
			[this] { resend(); });
	}

	void goBack() override {
		intro()->showStep(StepType::Phone, true);
	}

protected:
	void verify(const QString &code) override {
		setWorking(true);
		auto &data = state();
		intro()->account().auth().verifyCode(
			data.challenge.challengeId,
			code,
			data.freshKey,
			this,
			[this](const Api::VerifyOutcome &outcome) {
				state().freshKey = false;
				if (outcome.kind == Api::VerifyOutcome::Kind::TwoStep) {
					state().form = outcome.form;
					intro()->showStep(StepType::Password);
				} else {
					intro()->signedIn(outcome);
				}
			},
			[this](const Api::Error &error) {
				// The key was registered with the first attempt or never.
				const auto wrong = !error.offline()
					&& !error.rateLimited()
					&& !error.serverSide();
				showCodeError(
					Support::ErrorText(error, tr::lng_intro_wrong_code(tr::now)),
					wrong);
			});
	}

private:
	void resend() {
		if (working()) {
			return;
		}
		resendRow()->setBusy(true);
		intro()->account().auth().requestCode(state().phone, this, [this](
				const Api::OtpChallenge &challenge) {
			auto &data = state();
			data.challenge = challenge;
			data.resendAt = NowMs() + qint64(challenge.resendAfter) * 1000;
			resendRow()->setBusy(false);
			intro()->showToast(tr::lng_intro_new_code_was_sent(tr::now));
		}, [this](const Api::Error &error) {
			resendRow()->setBusy(false);
			showError(Support::ErrorText(error));
		});
	}

};

// The login ticket of the password / recovery steps has a lifetime: when it
// ends the sign-in starts over from the phone number.
class TicketWatch final {
public:
	TicketWatch(Step *step, Widget *intro) {
		_timer.setSingleShot(true);
		QObject::connect(&_timer, &QTimer::timeout, step, [intro] {
			intro->restartExpired();
		});
		restart(intro);
	}

	void restart(Widget *intro) {
		const auto left = intro->state().form.expiresAt - NowMs();
		if (intro->state().form.expiresAt > 0) {
			_timer.start(int(std::clamp<qint64>(left, 0, 24 * 3600 * 1000)));
		}
	}

private:
	QTimer _timer;

};

[[nodiscard]] bool TicketGone(const Api::Error &error) {
	return (error.code == u"two_step.ticket_invalid")
		|| (error.code == u"two_step.ticket_expired")
		|| (error.status == 410);
}

// ---- the two-step password

class PasswordStep final : public Step {
public:
	PasswordStep(QWidget *parent, Widget *intro)
	: Step(parent, intro)
	, _ticket(this, intro) {
		addTitle(
			tr::lng_intro_two_step_password(tr::now),
			tr::lng_intro_two_step_about(tr::now));
		const auto layout = content();
		_password = layout->add(Ui::object_ptr<Ui::PasswordInput>(
			layout,
			st::introInputField,
			tr::lng_intro_password(tr::now)));
		_hint = layout->add(
			Ui::object_ptr<HintLine>(layout),
			style::margins(0, st::introHintSkip, 0, 0));
		showHint();
		_submit = layout->add(
			Ui::object_ptr<Ui::RoundButton>(
				layout,
				tr::lng_continue(tr::now),
				st::introButton),
			style::margins(0, st::introButtonSkip, 0, 0));
		_submit->setClickedCallback([this] { submit(); });
		_forgot = layout->add(
			Ui::object_ptr<Ui::LinkButton>(
				layout,
				tr::lng_intro_forgot_password(tr::now),
				st::introLink),
			style::margins(0, st::introLinkSkip, 0, 0),
			style::al_top);
		_forgot->setClickedCallback([this] { forgot(); });

		QObject::connect(
			_password,
			&Ui::MaskedInputField::changed,
			this,
			[this] { showHint(); });
		QObject::connect(
			_password,
			&Ui::MaskedInputField::submitted,
			this,
			[this] { submit(); });
		QObject::connect(
			_password,
			&Ui::MaskedInputField::cancelled,
			this,
			[this] { goBack(); });
	}

	void setInnerFocus() override {
		_password->setFocusFast();
	}
	void goBack() override {
		intro()->showStep(StepType::Phone, true);
	}
	void submit() override {
		const auto password = _password->text();
		if (_working) {
			return;
		} else if (password.isEmpty()) {
			_password->showError();
			return;
		}
		setWorking(true);
		intro()->account().auth().signInWithPassword(
			state().form,
			password,
			this,
			[this](const Api::VerifyOutcome &outcome) {
				intro()->signedIn(outcome);
			},
			[this](const Api::Error &error) {
				setWorking(false);
				if (TicketGone(error)) {
					intro()->restartExpired();
				} else if (error.status == 403) {
					_password->showError();
					_hint->setError(tr::lng_intro_wrong_password(tr::now));
				} else {
					_hint->setError(Support::ErrorText(error));
				}
			});
	}

private:
	void showHint() {
		const auto &hint = state().form.hint;
		_hint->setText(hint.isEmpty()
			? QString()
			: tr::lng_intro_hint(tr::now, lt_hint, hint));
	}
	void setWorking(bool working) {
		_working = working;
		_submit->setLoading(working);
	}
	void forgot() {
		if (_working || _recovering) {
			return;
		} else if (state().form.recoveryEmail.isEmpty()) {
			intro()->showToast(tr::lng_intro_account_has_no_recovery_e(tr::now));
			return;
		}
		_recovering = true;
		intro()->account().auth().requestRecovery(state().form, this, [this](
				const Api::RecoveryChallenge &challenge) {
			auto &data = state();
			data.recovery = challenge;
			data.recoveryResendAt = NowMs()
				+ qint64(challenge.resendAfter) * 1000;
			// The ticket now lives as long as the mailed code.
			data.form.expiresAt = NowMs() + qint64(challenge.expiresIn) * 1000;
			intro()->showStep(StepType::Recovery);
		}, [this](const Api::Error &error) {
			_recovering = false;
			if (TicketGone(error)) {
				intro()->restartExpired();
			} else {
				_hint->setError(Support::ErrorText(error));
			}
		});
	}

	TicketWatch _ticket;
	Ui::PasswordInput *_password = nullptr;
	HintLine *_hint = nullptr;
	Ui::RoundButton *_submit = nullptr;
	Ui::LinkButton *_forgot = nullptr;
	bool _working = false;
	bool _recovering = false;

};

// ---- the recovery e-mail code

class RecoveryStep final : public CodeStepBase {
public:
	RecoveryStep(QWidget *parent, Widget *intro)
	: CodeStepBase(parent, intro)
	, _ticket(this, intro) {
		const auto email = state().recovery.email.isEmpty()
			? state().form.recoveryEmail
			: state().recovery.email;
		addTitle(
			tr::lng_intro_we_e_mailed_code_recovery(tr::now),
			email.isEmpty()
				? QString()
				: tr::lng_intro_recovery_sent_to(
					tr::now,
					lt_email,
					Isolated(email)));
		addCodeRows(
			state().recovery.codeLength,
			[this] { return state().recoveryResendAt; },
			[this] { resend(); });
	}

	void goBack() override {
		intro()->showStep(StepType::Password, true);
	}

protected:
	void verify(const QString &code) override {
		setWorking(true);
		intro()->account().auth().confirmRecovery(
			state().form,
			code,
			this,
			[this](const Api::VerifyOutcome &outcome) {
				intro()->signedIn(outcome);
			},
			[this](const Api::Error &error) {
				if (TicketGone(error)) {
					intro()->restartExpired();
					return;
				}
				const auto wrong = !error.offline()
					&& !error.rateLimited()
					&& !error.serverSide();
				showCodeError(
					Support::ErrorText(error, tr::lng_intro_wrong_code(tr::now)),
					wrong);
			});
	}

private:
	void resend() {
		if (working()) {
			return;
		}
		resendRow()->setBusy(true);
		intro()->account().auth().requestRecovery(state().form, this, [this](
				const Api::RecoveryChallenge &challenge) {
			auto &data = state();
			data.recovery = challenge;
			data.recoveryResendAt = NowMs()
				+ qint64(challenge.resendAfter) * 1000;
			// A new code: the countdown of the ticket starts again.
			data.form.expiresAt = NowMs() + qint64(challenge.expiresIn) * 1000;
			_ticket.restart(intro());
			resendRow()->setBusy(false);
			intro()->showToast(tr::lng_intro_new_code_was_sent(tr::now));
		}, [this](const Api::Error &error) {
			resendRow()->setBusy(false);
			if (TicketGone(error)) {
				intro()->restartExpired();
			} else {
				showError(Support::ErrorText(error));
			}
		});
	}

	TicketWatch _ticket;

};

// ---- the profile of a new account

class ProfileStep final : public Step {
public:
	ProfileStep(QWidget *parent, Widget *intro) : Step(parent, intro) {
		addTitle(
			tr::lng_intro_profile_title(tr::now),
			tr::lng_intro_profile_about(tr::now));
		const auto layout = content();
		_photo = layout->add(
			Ui::object_ptr<PhotoButton>(layout),
			style::margins(0, 0, 0, st::introPhotoSkip),
			style::al_top);
		_photo->setClickedCallback([this] { choosePhoto(); });

		_name = layout->add(
			Ui::object_ptr<Ui::InputField>(
				layout,
				st::introInputField,
				tr::lng_name(tr::now)),
			style::margins(0, 0, 0, st::introFieldSkip));
		_name->setMaxLength(std::max(
			intro->account().clientConfig().displayNameMaxChars(),
			1));
		_handle = layout->add(Ui::object_ptr<HandleField>(layout));
		_hint = layout->add(
			Ui::object_ptr<HintLine>(layout),
			style::margins(0, st::introHintSkip, 0, 0));
		_submit = layout->add(
			Ui::object_ptr<Ui::RoundButton>(
				layout,
				tr::lng_continue(tr::now),
				st::introButton),
			style::margins(0, st::introButtonSkip, 0, 0));
		_submit->setClickedCallback([this] { submit(); });
		// A way out for somebody who signed in with the wrong number.
		const auto signOut = layout->add(
			Ui::object_ptr<Ui::LinkButton>(
				layout,
				tr::lng_sign_out(tr::now),
				st::introLink),
			style::margins(0, st::introLinkSkip, 0, 0),
			style::al_top);
		signOut->setClickedCallback([this] {
			// The application returns to the welcome step when the session ends.
			this->intro()->account().logout();
		});

		_checkTimer.setSingleShot(true);
		_checkTimer.setInterval(kHandleCheckDelay);
		QObject::connect(&_checkTimer, &QTimer::timeout, this, [this] {
			checkHandle();
		});
		QObject::connect(_name, &Ui::InputField::submitted, this, [this] {
			_handle->input()->setFocusFast();
		});
		QObject::connect(_name, &Ui::InputField::changed, this, [this] {
			if (_errorShown) {
				refreshHandleHint();
			}
		});
		QObject::connect(
			_handle->input(),
			&Ui::MaskedInputField::changed,
			this,
			[this] { handleChanged(); });
		QObject::connect(
			_handle->input(),
			&Ui::MaskedInputField::submitted,
			this,
			[this] { submit(); });
	}

	~ProfileStep() {
		cancelUpload();
	}

	bool hasBack() const override {
		return false;
	}
	void setInnerFocus() override {
		_name->setFocusFast();
	}
	void submit() override {
		if (_working) {
			return;
		}
		const auto name = _name->text().simplified();
		const auto handle = _handle->text();
		if (name.isEmpty()) {
			_name->showError();
			showError(tr::lng_intro_name_required(tr::now));
			return;
		} else if (!Api::Auth::IsValidHandle(handle)) {
			_handle->showError();
			showError((handle.size() < 3)
				? tr::lng_intro_username_needs_at_least_3(tr::now)
				: tr::lng_intro_username_rules(tr::now));
			return;
		} else if (_handleState == HandleState::Taken) {
			_handle->showError();
			showError(tr::lng_intro_username_taken(tr::now));
			return;
		}
		setWorking(true);
		if (_photoState == PhotoState::Preparing
			|| _photoState == PhotoState::Uploading) {
			// The photo is still on its way: continue when it arrived.
			_waitingForPhoto = true;
			return;
		}
		complete();
	}

private:
	enum class HandleState {
		Empty,
		Invalid,
		Checking,
		Taken,
		Available,
		Failed,
	};
	enum class PhotoState {
		None,
		Preparing,
		Uploading,
		Done,
		Failed,
	};

	void setWorking(bool working) {
		_working = working;
		_submit->setLoading(working);
	}
	void showError(const QString &text) {
		_errorShown = true;
		_hint->setError(text);
	}

	void handleChanged() {
		_checkTimer.stop();
		_errorShown = false;
		++_checkGeneration;
		const auto handle = _handle->text();
		if (handle.isEmpty()) {
			_handleState = HandleState::Empty;
		} else if (!Api::Auth::IsValidHandle(handle)) {
			_handleState = HandleState::Invalid;
		} else {
			_handleState = HandleState::Checking;
			_checkTimer.start();
		}
		refreshHandleHint();
	}
	void refreshHandleHint() {
		_errorShown = false;
		switch (_handleState) {
		case HandleState::Empty:
			_hint->clear();
			break;
		case HandleState::Invalid:
			_hint->setError((_handle->text().size() < 3)
				? tr::lng_intro_username_needs_at_least_3(tr::now)
				: tr::lng_intro_username_rules(tr::now));
			break;
		case HandleState::Checking:
			_hint->setText(tr::lng_intro_checking_username(tr::now));
			break;
		case HandleState::Taken:
			_hint->setError(tr::lng_intro_username_taken(tr::now));
			break;
		case HandleState::Available:
			_hint->setText(
				tr::lng_intro_username_available(tr::now),
				HintLine::Kind::Ok);
			break;
		case HandleState::Failed:
			_hint->setText(
				tr::lng_intro_couldnt_check_username_checked_again(tr::now));
			break;
		}
	}
	void checkHandle() {
		const auto handle = _handle->text();
		const auto generation = _checkGeneration;
		intro()->account().auth().isHandleAvailable(handle, this, [=, this](
				bool available) {
			if (generation != _checkGeneration) {
				return; // the text changed meanwhile
			}
			_handleState = available
				? HandleState::Available
				: HandleState::Taken;
			if (!_errorShown) {
				refreshHandleHint();
			}
		}, [=, this](const Api::Error &) {
			if (generation != _checkGeneration) {
				return;
			}
			_handleState = HandleState::Failed;
			if (!_errorShown) {
				refreshHandleHint();
			}
		});
	}

	void choosePhoto() {
		if (_working) {
			return;
		}
		const auto intro = this->intro();
		Boxes::PickAndCropPhoto(this, [intro](
				Ui::object_ptr<Ui::BoxContent> box) {
			intro->show(std::move(box));
		}, [this](const QImage &square) {
			photoChosen(square);
		}, [intro] {
			intro->showToast(tr::lng_intro_photo_not_picture(tr::now));
		});
	}
	void photoChosen(const QImage &square) {
		cancelUpload();
		_photo->setImage(square);
		_photo->setUploading(true);
		_photoState = PhotoState::Preparing;
		_mediaId = QString();
		const auto generation = ++_photoGeneration;
		// The upload runs in the background while the name is being typed.
		Boxes::SaveCroppedPhoto(square, this, [=, this](const QString &path) {
			if (generation != _photoGeneration) {
				QFile::remove(path);
				return;
			}
			const auto session = Core::App().session();
			if (path.isEmpty() || !session) {
				photoFinished(false);
				return;
			}
			_photoPath = path;
			_photoState = PhotoState::Uploading;
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
				if (generation == _photoGeneration) {
					_mediaId = result.mediaId;
					photoFinished(!_mediaId.isEmpty());
				}
			};
			callbacks.fail = [=, this](const Api::Error &error) {
				if (generation == _photoGeneration) {
					qWarning(
						"Intro: the photo upload failed: %s",
						qPrintable(error.forLog()));
					photoFinished(false);
				}
			};
			_uploadSession = session;
			_uploadId = session->data().upload().start(
				request,
				this,
				std::move(callbacks));
		});
	}
	void photoFinished(bool ok) {
		_uploadId = 0;
		_photoState = ok ? PhotoState::Done : PhotoState::Failed;
		_photo->setUploading(false);
		removePhotoFile();
		if (!ok) {
			_photo->setImage(QImage());
			intro()->showToast(tr::lng_chat_upload_failed(tr::now));
		}
		if (std::exchange(_waitingForPhoto, false)) {
			// Without the photo when it failed: it can be set later.
			complete();
		}
	}
	void cancelUpload() {
		++_photoGeneration;
		if (_uploadId && _uploadSession) {
			_uploadSession->data().upload().cancel(_uploadId);
		}
		_uploadId = 0;
		removePhotoFile();
	}
	void removePhotoFile() {
		if (!_photoPath.isEmpty()) {
			QFile::remove(std::exchange(_photoPath, QString()));
		}
	}

	void complete() {
		const auto name = _name->text().simplified();
		const auto handle = _handle->text();
		intro()->account().auth().completeProfile(
			name,
			handle,
			_mediaId,
			this,
			[this](const Data::Profile &profile) {
				if (const auto session = Core::App().session()) {
					if (profile.valid()) {
						session->data().users().setMe(profile);
					} else {
						session->data().users().refreshMe();
					}
				}
				auto outcome = Api::VerifyOutcome();
				outcome.needsProfile = false;
				intro()->signedIn(outcome);
			},
			[this](const Api::Error &error) {
				setWorking(false);
				if (error.status == 409 || error.code == u"handle.taken") {
					_handleState = HandleState::Taken;
					_handle->showError();
					showError(tr::lng_intro_username_taken(tr::now));
				} else {
					showError(Support::ErrorText(error));
				}
			});
	}

	PhotoButton *_photo = nullptr;
	Ui::InputField *_name = nullptr;
	HandleField *_handle = nullptr;
	HintLine *_hint = nullptr;
	Ui::RoundButton *_submit = nullptr;
	QTimer _checkTimer;
	HandleState _handleState = HandleState::Empty;
	int _checkGeneration = 0;
	bool _errorShown = false;
	bool _working = false;

	PhotoState _photoState = PhotoState::None;
	int _photoGeneration = 0;
	QString _photoPath;
	QString _mediaId;
	QPointer<Main::Session> _uploadSession;
	Data::Upload::Id _uploadId = 0;
	bool _waitingForPhoto = false;

};

} // namespace

HintLine::HintLine(QWidget *parent, style::align align)
: RpWidget(parent)
, _align(align) {
	resize(width(), st::introHintStyle.lineHeight);
}

void HintLine::setText(const QString &text, Kind kind) {
	if (_text.toString() == text && _kind == kind) {
		return;
	}
	_kind = kind;
	_text.setText(st::introHintStyle, text);
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
}

int HintLine::resizeGetHeight(int newWidth) {
	const auto line = int(st::introHintStyle.lineHeight);
	return _text.isEmpty()
		? line
		: std::max(_text.countHeight(std::max(newWidth, 1)), line);
}

void HintLine::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	if (_text.isEmpty()) {
		return;
	}
	auto p = Painter(this);
	switch (_kind) {
	case Kind::Error: p.setPen(st::introHintErrorFg); break;
	case Kind::Ok: p.setPen(st::introHintOkFg); break;
	case Kind::Hint: p.setPen(st::introHintFg); break;
	}
	_text.draw(p, 0, 0, width(), _align);
}

Step::Step(QWidget *parent, Widget *intro)
: RpWidget(parent)
, _intro(intro)
, _content(new Ui::VerticalLayout(this)) {
	setFocusPolicy(Qt::StrongFocus);
	QObject::connect(_content, &Ui::RpWidget::heightChanged, this, [this](
			int height) {
		if (this->height() != height) {
			resize(width(), height);
		}
	});
}

void Step::addTitle(const QString &title, const QString &about) {
	_content->add(
		Ui::object_ptr<Ui::FlatLabel>(_content, title, st::introTitle),
		style::margins(
			0,
			0,
			0,
			about.isEmpty() ? st::introTitleSkip : st::introTitleSkip));
	if (!about.isEmpty()) {
		_content->add(
			Ui::object_ptr<Ui::FlatLabel>(_content, about, st::introAbout),
			style::margins(0, 0, 0, st::introAboutSkip));
	}
}

int Step::resizeGetHeight(int newWidth) {
	_content->resizeToWidth(newWidth);
	_content->move(0, 0);
	return _content->height();
}

void Step::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
		submit();
	} else if (e->key() == Qt::Key_Escape && hasBack()) {
		goBack();
	} else {
		RpWidget::keyPressEvent(e);
	}
}

Step *CreateStep(StepType type, QWidget *parent, Widget *intro) {
	switch (type) {
	case StepType::Welcome: return new WelcomeStep(parent, intro);
	case StepType::Phone: return new PhoneStep(parent, intro);
	case StepType::Code: return new CodeStep(parent, intro);
	case StepType::Password: return new PasswordStep(parent, intro);
	case StepType::Recovery: return new RecoveryStep(parent, intro);
	case StepType::Profile: return new ProfileStep(parent, intro);
	}
	return new WelcomeStep(parent, intro);
}

void ShowCountryBox(
		Widget *intro,
		std::function<void(const QString &iso)> chosen) {
	intro->show(Ui::Box<Boxes::CountrySelectBox>(
		intro->state().countryIso,
		[chosen = std::move(chosen)](const Lang::Country &country) {
			if (chosen) {
				chosen(country.iso);
			}
		}));
}

} // namespace Intro
