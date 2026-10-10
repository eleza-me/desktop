#include "conversation/conversation_boxes.h"

#include "boxes/report_box.h"
#include "conversation/conversation_card.h"
#include "conversation/conversation_controls.h"
#include "conversation/conversation_radar.h"
#include "conversation/conversation_radar_model.h"
#include "conversation/conversation_time.h"
#include "data/data_chats.h"
#include "data/data_conversations.h"
#include "data/data_feed.h"
#include "data/data_graph.h"
#include "data/data_session.h"
#include "data/data_transport.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/chat_text.h"
#include "support/error_text.h"
#include "support/media_image.h"
#include "ui/effects/radial_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_conversation.h"

#include <QtCore/QDateTime>
#include <QtCore/QUuid>

namespace Conversation {
namespace {

constexpr auto kFindChatTimeout = 7000;
// Without the connection the row cannot be announced: the inbox is read
// (once) sooner.
constexpr auto kFindChatOfflineTimeout = 2400;
constexpr auto kInviteMaxChars = 500;

[[nodiscard]] Ui::MenuItem DestructiveItem(
		const QString &text,
		std::function<void()> callback,
		const style::icon *icon) {
	auto result = Ui::MenuItem();
	result.text = text;
	result.callback = std::move(callback);
	result.icon = icon;
	result.destructive = true;
	return result;
}

[[nodiscard]] style::margins RowMargins(int bottom) {
	const auto &padding = st::conversationBoxPadding;
	return style::margins(padding.left(), 0, padding.right(), bottom);
}

// The box follows the height of its rows (plus the paddings around them);
// taller than the window, it scrolls.
void FitBoxToContent(
		Ui::BoxContent *box,
		Ui::VerticalLayout *content,
		Ui::object_ptr<Ui::VerticalLayout> owned) {
	const auto width = int(st::conversationBoxWidth);
	const auto top = st::conversationBoxPadding.top();
	const auto bottom = st::conversationBoxPadding.bottom();
	content->resizeToWidth(width);
	box->setDimensions(width, top + content->height() + bottom);
	QObject::connect(content, &Ui::RpWidget::heightChanged, box, [=](int height) {
		box->setDimensions(width, top + height + bottom);
	});
	box->setInnerWidget(std::move(owned), top, bottom);
}

// The hint under the topic field with the «۱۲/۱۲۰» counter at the end.
class HintRow final : public Ui::RpWidget {
public:
	HintRow(QWidget *parent, const QString &text)
	: RpWidget(parent)
	, _label(new Ui::FlatLabel(this, text, st::conversationHint)) {
	}

	void setCounter(int count, int max) {
		_text = Lang::Num(count) + QLatin1Char('/') + Lang::Num(max);
		_over = (count > max);
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto counter = st::conversationCounterFont->width(
			QStringLiteral("0000/0000"));
		_label->resizeToWidth(std::max(
			newWidth - counter - int(st::conversationCardSkip),
			1));
		_label->moveToLeft(0, 0, newWidth);
		return std::max(_label->height(), st::conversationCounterFont->height);
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		p.setFont(st::conversationCounterFont);
		p.setPen(_over
			? st::conversationCounterFgError
			: st::conversationCounterFg);
		p.drawTextRight(0, 0, width(), _text);
	}

private:
	Ui::FlatLabel *_label = nullptr;
	QString _text;
	bool _over = false;

};

// ---- «گفتگوی تازه»

class NewConversationBox final : public Ui::BoxContent {
public:
	NewConversationBox(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &prefill)
	: BoxContent(parent)
	, _controller(controller)
	, _prefill(prefill) {
	}

	void setInnerFocus() override {
		_topic->setFocusFast();
	}

protected:
	void prepare() override;

private:
	void updateCounter();
	void showProblem(const QString &text);
	void applyDefaultDuration(const QString &preferred);
	void submit();

	Window::SessionController *_controller = nullptr;
	QString _prefill;
	Data::ConversationConfig _config;
	QStringList _durations;
	Ui::VerticalLayout *_content = nullptr;
	Ui::InputField *_topic = nullptr;
	HintRow *_hint = nullptr;
	ChipFlow *_categories = nullptr;
	IdentityPicker *_identity = nullptr;
	Ui::FlatLabel *_note = nullptr;
	ChipFlow *_duration = nullptr;
	Ui::FlatLabel *_error = nullptr;
	Ui::RoundButton *_submit = nullptr;
	bool _durationTouched = false;
	bool _sending = false;

};

void NewConversationBox::prepare() {
	auto &data = _controller->session().data();
	_config = data.conversations().config();
	_durations = SortedDurations(_config.durations);

	setTitle(tr::lng_conv_new_conversation(tr::now));
	auto owned = Ui::object_ptr<Ui::VerticalLayout>(this);
	_content = owned.data();
	const auto section = int(st::conversationBoxSectionSkip);
	const auto labelSkip = int(st::conversationBoxLabelSkip);

	_topic = _content->add(
		Ui::object_ptr<Ui::InputField>(
			_content,
			st::conversationTopicField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_conv_what_do_want_talk_about(tr::now),
			_prefill),
		RowMargins(st::conversationHintSkip));
	_topic->setSubmitSettings(Ui::InputField::SubmitSettings::Both);
	_hint = _content->add(
		Ui::object_ptr<HintRow>(
			_content,
			tr::lng_conv_one_sentence_enough_listener_only(tr::now)),
		RowMargins(st::conversationHintSkip));
	_error = _content->add(
		Ui::object_ptr<Ui::FlatLabel>(_content, st::conversationError),
		RowMargins(0));
	_error->hide();
	_content->add(
		Ui::object_ptr<Ui::FixedHeightWidget>(
			_content,
			section - int(st::conversationHintSkip)));

	if (!_config.categories.isEmpty()) {
		_content->add(
			Ui::object_ptr<Ui::FlatLabel>(
				_content,
				tr::lng_conv_category(tr::now),
				st::conversationFormLabel),
			RowMargins(labelSkip));
		_categories = _content->add(
			Ui::object_ptr<ChipFlow>(_content),
			RowMargins(section));
		auto items = std::vector<ChipFlow::Item>();
		for (const auto &category : _config.categories) {
			items.push_back({ category.slug, category.title });
		}
		_categories->setItems(items);
		_categories->setActive(_config.categories.front().slug);
	}

	_content->add(
		Ui::object_ptr<Ui::FlatLabel>(
			_content,
			tr::lng_conv_as_whom(tr::now),
			st::conversationFormLabel),
		RowMargins(labelSkip));
	_identity = _content->add(
		Ui::object_ptr<IdentityPicker>(_content, &_controller->session()),
		RowMargins(labelSkip));
	_note = _content->add(
		Ui::object_ptr<Ui::FlatLabel>(
			_content,
			tr::lng_conv_anonymous_conversation_name_stays_hidden(tr::now),
			st::conversationNote),
		RowMargins(section));
	_identity->setChangedCallback([this](bool anonymous) {
		_note->setText(anonymous
			? tr::lng_conv_anonymous_conversation_name_stays_hidden(tr::now)
			: tr::lng_conversation_named_note(tr::now));
	});

	if (!_durations.isEmpty()) {
		_content->add(
			Ui::object_ptr<Ui::FlatLabel>(
				_content,
				tr::lng_conv_open_how_long(tr::now),
				st::conversationFormLabel),
			RowMargins(labelSkip));
		_duration = _content->add(
			Ui::object_ptr<ChipFlow>(_content),
			RowMargins(section));
		auto items = std::vector<ChipFlow::Item>();
		for (const auto &duration : _durations) {
			items.push_back({ duration, DurationTitle(duration) });
		}
		_duration->setItems(items);
		_duration->setChangedCallback([this](const QString &) {
			_durationTouched = true;
		});
	}

	// The user's default lifetime («پیام‌ها و گفتگو» settings), when it is
	// one of the choices; the longest one otherwise.
	auto &graph = data.graph();
	applyDefaultDuration(graph.settingsLoaded()
		? graph.settingValue(u"messages.default_expiry"_s).toString()
		: QString());
	if (!graph.settingsLoaded()) {
		graph.loadSettings(false, this, [this](const QJsonObject &) {
			if (!_durationTouched) {
				applyDefaultDuration(_controller->session().data().graph()
					.settingValue(u"messages.default_expiry"_s).toString());
			}
		}, [](const Api::Error &) {
		});
	}

	QObject::connect(_topic, &Ui::InputField::changed, this, [this] {
		updateCounter();
		showProblem(QString());
	});
	QObject::connect(_topic, &Ui::InputField::submitted, this, [this] {
		submit();
	});
	QObject::connect(_topic, &Ui::InputField::cancelled, this, [this] {
		closeBox();
	});
	updateCounter();

	_submit = addButton(
		tr::lng_conv_start_conversation(tr::now),
		[this] { submit(); },
		st::defaultRoundButton);
	_submit->setIcon(&st::conversationCreateIcon);
	addButton(tr::lng_cancel(tr::now), [this] { closeBox(); });

	FitBoxToContent(this, _content, std::move(owned));
}

void NewConversationBox::applyDefaultDuration(const QString &preferred) {
	if (_duration) {
		_duration->setActive(DefaultDuration(
			_durations,
			preferred,
			_config.defaultDuration));
	}
}

void NewConversationBox::updateCounter() {
	_hint->setCounter(
		int(NormalizedTopic(_topic->text()).size()),
		_config.topicMaxChars);
}

void NewConversationBox::showProblem(const QString &text) {
	if (text.isEmpty()) {
		if (!_error->isHidden()) {
			_error->hide();
		}
		return;
	}
	_error->setText(text);
	_error->show();
}

void NewConversationBox::submit() {
	if (_sending) {
		return;
	}
	const auto problem = CheckTopic(_topic->text(), _config.topicMaxChars);
	if (problem == TopicProblem::Empty) {
		_topic->showError();
		_topic->setFocusFast();
		showProblem(tr::lng_conversation_topic_empty(tr::now));
		return;
	} else if (problem == TopicProblem::TooLong) {
		_topic->showError();
		_topic->setFocusFast();
		showProblem(tr::lng_conversation_topic_too_long(
			tr::now,
			lt_count,
			_config.topicMaxChars));
		return;
	}
	auto request = Data::NewConversation();
	request.topic = NormalizedTopic(_topic->text());
	request.category = (_categories && !_categories->active().isEmpty())
		? _categories->active()
		: u"other"_s;
	request.duration = (_duration && !_duration->active().isEmpty())
		? _duration->active()
		: _config.defaultDuration;
	request.masked = _identity->anonymous();
	_sending = true;
	_submit->setLoading(true);
	showProblem(QString());
	const auto controller = _controller;
	controller->session().data().conversations().create(request, this, [=, this](
			const Data::Conversation &conversation) {
		Q_EMIT Events()->topicCreated(conversation.id);
		// The box dies here: only locals below.
		closeBox();
		controller->showMainSection(
			RadarFactory(conversation.id, conversation),
			{ .stack = false, .tab = Window::Tab::Chats });
	}, [this](const Api::Error &error) {
		_sending = false;
		_submit->setLoading(false);
		showProblem(Support::ErrorText(
			error,
			tr::lng_conv_couldnt_open_try_again(tr::now)));
	});
}

// ---- «پیوستن به گفتگو»

class SpeakerRow final : public Ui::RpWidget {
public:
	SpeakerRow(
		QWidget *parent,
		Main::Session *session,
		const Data::Participant &speaker)
	: RpWidget(parent)
	, _session(session)
	, _speaker(speaker) {
		const auto size = int(st::conversationJoinSpeakerSize);
		_userpic = new Support::UserpicButton(
			this,
			session,
			speaker.userId,
			size);
		_userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
		if (masked()) {
			_userpic->setSpecial(Ui::UserpicKind::Anonymous, name(), speaker.ref);
		} else {
			QObject::connect(
				&session->data().users(),
				&Data::Users::changed,
				this,
				[this] { update(); });
		}
		resize(width(), st::conversationJoinSpeakerHeight);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto size = int(st::conversationJoinSpeakerSize);
		_userpic->moveToLeft(
			0,
			(int(st::conversationJoinSpeakerHeight) - size) / 2,
			newWidth);
		return st::conversationJoinSpeakerHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto left = int(st::conversationJoinSpeakerSize)
			+ int(st::conversationJoinSpeakerSkip);
		const auto available = std::max(width() - left, 0);
		const auto nameFont = st::conversationJoinSpeakerNameFont;
		const auto roleFont = st::conversationJoinSpeakerRoleFont;
		const auto total = nameFont->height + roleFont->height;
		const auto top = (height() - total) / 2;
		p.setFont(nameFont);
		p.setPen(st::textPrimary);
		p.drawTextLeft(left, top, width(), nameFont->elided(name(), available));
		const auto &icon = st::conversationJoinSpeakerRoleIcon;
		icon.paint(
			p,
			left,
			top + nameFont->height + (roleFont->height - icon.height()) / 2,
			width());
		p.setFont(roleFont);
		p.setPen(st::textTertiary);
		p.drawTextLeft(
			left + icon.width() + 4,
			top + nameFont->height,
			width(),
			tr::lng_conv_speaker(tr::now));
	}

private:
	[[nodiscard]] bool masked() const {
		return _speaker.masked || _speaker.userId.isEmpty();
	}
	[[nodiscard]] QString name() const {
		if (masked()) {
			return _speaker.maskName.isEmpty()
				? tr::lng_anonymous(tr::now)
				: _speaker.maskName;
		}
		const auto user = _session->data().users().get(_speaker.userId);
		return user.deleted
			? tr::lng_deleted_account(tr::now)
			: user.displayName;
	}

	Main::Session *_session = nullptr;
	Data::Participant _speaker;
	Support::UserpicButton *_userpic = nullptr;

};

class CenteredSpinner final : public Ui::RpWidget {
public:
	CenteredSpinner(QWidget *parent, int height)
	: RpWidget(parent)
	, _spinner(new Ui::LoadingSpinner(
		this,
		st::conversationSpinnerSize,
		st::brandPrimary)) {
		const auto size = int(st::conversationSpinnerSize);
		_spinner->resize(size, size);
		resize(width(), height);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		_spinner->move(
			(newWidth - _spinner->width()) / 2,
			(height() - _spinner->height()) / 2);
		return height();
	}

private:
	Ui::LoadingSpinner *_spinner = nullptr;

};

class JoinBox final : public Ui::BoxContent {
public:
	JoinBox(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &id)
	: BoxContent(parent)
	, _controller(controller)
	, _id(id) {
	}

protected:
	void prepare() override;

private:
	void load();
	void loadFromList(const Api::Error &original);
	void loaded(const Data::Conversation &conversation);
	void checkReady();
	void showLoading();
	void showFailed(const QString &text);
	void showReady();
	void showMenu(QWidget *anchor);
	void join();
	[[nodiscard]] Data::Session &data() const {
		return _controller->session().data();
	}

	Window::SessionController *_controller = nullptr;
	QString _id;
	Ui::VerticalLayout *_content = nullptr;
	std::optional<Data::Conversation> _conversation;
	std::optional<bool> _allowsMask;
	IdentityPicker *_identity = nullptr;
	Ui::FlatLabel *_seen = nullptr;
	Ui::RoundButton *_listen = nullptr;
	bool _anonymous = true;
	bool _joining = false;
	bool _failed = false;

};

void JoinBox::prepare() {
	setTitle(tr::lng_eli_join_conversation(tr::now));
	auto owned = Ui::object_ptr<Ui::VerticalLayout>(this);
	_content = owned.data();
	FitBoxToContent(this, _content, std::move(owned));
	showLoading();

	auto &graph = data().graph();
	const auto read = [this] {
		const auto value = data().graph().settingValue(
			u"privacy.conversation_masked_join_allowed"_s);
		_allowsMask = (value.isUndefined() || value.isNull())
			? true
			: value.toBool();
		checkReady();
	};
	if (graph.settingsLoaded()) {
		read();
	} else {
		graph.loadSettings(false, this, [=](const QJsonObject &) {
			read();
		}, [this](const Api::Error &) {
			_allowsMask = true;
			checkReady();
		});
	}
	load();
}

void JoinBox::load() {
	data().conversations().get(_id, this, [this](
			const Data::Conversation &conversation) {
		loaded(conversation);
	}, [this](const Api::Error &error) {
		loadFromList(error);
	});
}

// The view could not be read: the open list may still know the topic.
void JoinBox::loadFromList(const Api::Error &original) {
	if (original.notFound() || original.code == u"conversation.not_open") {
		showFailed(tr::lng_conv_may_have_ended_found_its(tr::now));
		return;
	}
	data().conversations().loadOpen(QString(), QString(), this, [this](
			const Data::OpenConversationsPage &page) {
		for (const auto &item : page.conversations) {
			if (item.id != _id) {
				continue;
			}
			auto conversation = Data::Conversation();
			conversation.id = item.id;
			conversation.topic = item.topic;
			conversation.category = item.category;
			conversation.state = u"open"_s;
			conversation.expiresAt = item.expiresAt;
			conversation.speaker.ref = item.speakerId.isEmpty()
				? u"p"_s
				: item.speakerId;
			conversation.speaker.masked = item.speakerMasked;
			conversation.speaker.maskName = item.maskName;
			conversation.speaker.maskAvatar = item.maskAvatar;
			conversation.speaker.userId = item.speakerId;
			conversation.speaker.role = u"speaker"_s;
			conversation.speaker.me = (item.speakerId == data().userId());
			loaded(conversation);
			return;
		}
		showFailed(tr::lng_conv_may_have_ended_found_its(tr::now));
	}, [this](const Api::Error &error) {
		showFailed(Support::ErrorText(error, tr::lng_error_load(tr::now)));
	});
}

void JoinBox::loaded(const Data::Conversation &conversation) {
	if (conversation.id.isEmpty()) {
		showFailed(tr::lng_conv_may_have_ended_found_its(tr::now));
		return;
	}
	const auto mine = conversation.speaker.me
		|| (conversation.myRole == u"speaker")
		|| (conversation.myRole == u"listener");
	if (mine) {
		// My own conversation: there is nothing to join.
		const auto controller = _controller;
		const auto id = _id;
		const auto waiting = (conversation.myRole != u"listener")
			&& (PhaseOf(
				conversation,
				QDateTime::currentMSecsSinceEpoch()) == RadarPhase::Waiting);
		// The box dies here: only locals below.
		closeBox();
		if (waiting) {
			controller->showMainSection(
				RadarFactory(id, conversation),
				{ .tab = Window::Tab::Chats });
		} else {
			controller->activateTab(Window::Tab::Chats);
			OpenChatOf(controller, id);
		}
		return;
	}
	const auto open = (conversation.state == u"open")
		|| (conversation.state == u"waiting");
	if (conversation.listener || !open) {
		showFailed(tr::lng_conv_may_have_ended_found_its(tr::now));
		return;
	}
	_conversation = conversation;
	if (!conversation.speaker.userId.isEmpty()) {
		data().users().request(conversation.speaker.userId);
	}
	checkReady();
}

void JoinBox::checkReady() {
	if (_conversation && _allowsMask && !_failed) {
		showReady();
	}
}

void JoinBox::showLoading() {
	_content->clear();
	_content->add(Ui::object_ptr<CenteredSpinner>(
		_content,
		st::conversationJoinMinHeight));
	clearButtons();
	addButton(tr::lng_cancel(tr::now), [this] { closeBox(); });
}

void JoinBox::showFailed(const QString &text) {
	_failed = true;
	_listen = nullptr;
	_identity = nullptr;
	_seen = nullptr;
	_content->clear();
	auto descriptor = Ui::EmptyPlaceholderDescriptor();
	descriptor.icon = &st::conversationStateIcon;
	descriptor.title = tr::lng_conv_conversation_no_longer_open(tr::now);
	descriptor.text = text;
	_content->add(
		Ui::object_ptr<Ui::EmptyPlaceholder>(_content, std::move(descriptor)));
	clearButtons();
	addButton(tr::lng_close(tr::now), [this] { closeBox(); });
	Q_EMIT Events()->topicHidden(_id);
}

void JoinBox::showReady() {
	const auto &conversation = *_conversation;
	const auto section = int(st::conversationBoxSectionSkip);
	const auto labelSkip = int(st::conversationBoxLabelSkip);
	_content->clear();
	const auto header = _content->add(
		Ui::object_ptr<ExpiryHeader>(
			_content,
			conversation.expiresAt
				? conversation.expiresAt
				: (QDateTime::currentSecsSinceEpoch()),
			data().conversations().categoryTitle(conversation.category)),
		RowMargins(labelSkip));
	header->setMoreCallback([this](QWidget *anchor) {
		showMenu(anchor);
	});
	const auto topic = _content->add(
		Ui::object_ptr<Ui::FlatLabel>(
			_content,
			conversation.topic,
			st::conversationJoinTopic),
		RowMargins(section));
	topic->setAutoDirection(true);
	topic->setSelectable(true);
	_content->add(
		Ui::object_ptr<SpeakerRow>(
			_content,
			&_controller->session(),
			conversation.speaker),
		RowMargins(labelSkip));
	_content->add(
		Ui::object_ptr<Ui::Divider>(_content),
		RowMargins(labelSkip));
	_content->add(
		Ui::object_ptr<Ui::FlatLabel>(
			_content,
			tr::lng_conv_only_one_person_can_listen(tr::now),
			st::conversationBody),
		RowMargins(section));
	_anonymous = *_allowsMask;
	if (*_allowsMask) {
		_identity = _content->add(
			Ui::object_ptr<IdentityPicker>(_content, &_controller->session()),
			RowMargins(section));
		_identity->setChangedCallback([this](bool anonymous) {
			_anonymous = anonymous;
			_seen->setText(anonymous
				? tr::lng_conversation_join_masked_note(tr::now)
				: tr::lng_conversation_join_named_note(tr::now));
		});
	}
	_content->add(
		Ui::object_ptr<Ui::FlatLabel>(
			_content,
			tr::lng_conv_others_see_like(tr::now),
			st::conversationFormLabel),
		RowMargins(labelSkip / 2));
	_seen = _content->add(
		Ui::object_ptr<Ui::FlatLabel>(
			_content,
			_anonymous
				? tr::lng_conversation_join_masked_note(tr::now)
				: tr::lng_conversation_join_named_note(tr::now),
			st::conversationNote),
		RowMargins(labelSkip));
	clearButtons();
	_listen = addButton(
		tr::lng_conv_listen(tr::now),
		[this] { join(); },
		st::defaultRoundButton);
	_listen->setIcon(&st::conversationJoinListenIcon);
	addButton(tr::lng_cancel(tr::now), [this] { closeBox(); });
}

void JoinBox::showMenu(QWidget *anchor) {
	const auto menu = new Ui::PopupMenu(this);
	const auto controller = _controller;
	const auto id = _id;
	const auto weak = QPointer<JoinBox>(this);
	menu->addAction(tr::lng_conv_dont_show_again(tr::now), [=] {
		DismissTopic(controller, id);
		if (weak) {
			weak->closeBox();
		}
	}, &st::conversationMenuHide);
	menu->addItem(DestructiveItem(
		tr::lng_report(tr::now),
		[=] {
			if (weak) {
				weak->closeBox();
			}
			ShowReportBox(controller, id);
		},
		&st::menuIconReport));
	menu->popupAnchored(anchor);
}

void JoinBox::join() {
	if (_joining || !_conversation) {
		return;
	}
	_joining = true;
	_listen->setLoading(true);
	const auto controller = _controller;
	const auto id = _id;
	data().conversations().join(id, _anonymous, this, [=, this](
			const Data::Conversation &) {
		Q_EMIT Events()->topicHidden(id);
		// The box dies here: only locals below.
		closeBox();
		controller->activateTab(Window::Tab::Chats);
		OpenChatOf(controller, id);
	}, [=, this](const Api::Error &error) {
		_joining = false;
		if (_listen) {
			_listen->setLoading(false);
		}
		if (error.code == u"conversation.not_open" || error.notFound()) {
			showFailed(tr::lng_conv_may_have_ended_found_its(tr::now));
			return;
		}
		Support::ShowError(
			controller,
			error,
			tr::lng_conv_couldnt_join(tr::now));
	});
}

} // namespace

Notifier *Events() {
	static const auto result = new Notifier();
	return result;
}

void ShowNewConversationBox(
		Window::SessionController *controller,
		const QString &prefill) {
	const auto max = controller->session().data().conversations().config()
		.topicMaxChars;
	controller->show(Ui::Box<NewConversationBox>(
		controller,
		(max > 0) ? prefill.trimmed().left(max) : prefill.trimmed()));
}

void ShowJoinBox(Window::SessionController *controller, const QString &id) {
	controller->show(Ui::Box<JoinBox>(controller, id));
}

void DismissTopic(Window::SessionController *controller, const QString &id) {
	controller->session().data().conversations().dismiss(id);
	Q_EMIT Events()->topicHidden(id);
	controller->showToast(tr::lng_conv_topic_wont_be_shown_again(tr::now));
}

void ShowReportBox(Window::SessionController *controller, const QString &id) {
	// The form is shared with the feed and the profile.
	auto args = Boxes::ReportArgs();
	args.kind = u"conversation"_s;
	args.id = id;
	args.sent = [=] {
		// A reported topic is not shown to me again.
		controller->session().data().conversations().dismiss(id);
		Q_EMIT Events()->topicHidden(id);
	};
	Boxes::ShowReportBox(controller, std::move(args));
}

void ShowInviteBox(Window::SessionController *controller, const QString &id) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_eli_invite_conversation(tr::now));
		box->setWidth(st::conversationBoxWidth);
		const auto field = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::conversationReportField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_conversation_invite_placeholder(tr::now)));
		field->setMaxLength(kInviteMaxChars);
		field->setSubmitSettings(Ui::InputField::SubmitSettings::CtrlEnter);
		box->addRow(Ui::object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_conversation_invite_about(tr::now),
			st::conversationHint));
		box->setFocusCallback([=] { field->setFocusFast(); });

		struct State {
			QString key = QUuid::createUuid().toString(QUuid::WithoutBraces);
			bool sending = false;
		};
		const auto state = std::make_shared<State>();
		const auto send = std::make_shared<Ui::RoundButton*>(nullptr);
		const auto submit = [=] {
			if (state->sending) {
				return;
			}
			state->sending = true;
			(*send)->setLoading(true);
			auto post = Data::CreatePost();
			post.type = u"conversation_invite"_s;
			post.text = field->text().trimmed();
			post.conversation = id;
			controller->session().data().feed().createPost(
				post,
				state->key,
				box,
				[=](const Data::Post &) {
					controller->showToast(
						tr::lng_conversation_invite_published(tr::now));
					box->closeBox();
				},
				[=](const Api::Error &error) {
					state->sending = false;
					(*send)->setLoading(false);
					Support::ShowError(
						controller,
						error,
						tr::lng_conversation_invite_failed(tr::now));
				});
		};
		*send = box->addButton(
			tr::lng_conversation_invite_publish(tr::now),
			submit,
			st::defaultRoundButton);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
		QObject::connect(field, &Ui::InputField::submitted, box, submit);
	}));
}

void ConfirmEnd(
		Window::SessionController *controller,
		const QString &id,
		std::function<void()> done) {
	auto args = Ui::ConfirmBoxArgs();
	args.text = tr::lng_conv_topic_removed_from_list_nobody(tr::now);
	args.title = tr::lng_conv_close_sure(tr::now);
	args.confirmText = tr::lng_conv_close_conversation(tr::now);
	args.destructive = true;
	args.confirmed = [=](std::function<void()> close) {
		controller->session().data().conversations().end(id, controller, [=] {
			Q_EMIT Events()->topicHidden(id);
			if (done) {
				done();
			}
		}, [=](const Api::Error &error) {
			Support::ShowError(controller, error);
		});
		close();
	};
	controller->show(Ui::MakeConfirmBox(std::move(args)));
}

void ShowTopicMenu(
		QWidget *parent,
		Window::SessionController *controller,
		const Card &card,
		const QPoint &globalPosition) {
	const auto menu = new Ui::PopupMenu(parent);
	const auto id = card.item.id;
	if (card.own) {
		menu->addAction(tr::lng_conversation_view(tr::now), [=] {
			OpenConversation(controller, id);
		}, &st::conversationMenuOpen);
		menu->addItem(DestructiveItem(
			tr::lng_conv_close_conversation(tr::now),
			[=] { ConfirmEnd(controller, id); },
			&st::conversationMenuEnd));
	} else {
		menu->addAction(tr::lng_eli_join_conversation(tr::now), [=] {
			ShowJoinBox(controller, id);
		}, &st::conversationMenuOpen);
		if (!card.anonymous && !card.item.speakerId.isEmpty()) {
			const auto user = card.item.speakerId;
			menu->addAction(tr::lng_conversation_speaker_profile(tr::now), [=] {
				controller->openRoute(Window::Routes::User(user));
			}, &st::conversationMenuProfile);
		}
		menu->addSeparator();
		menu->addAction(tr::lng_conv_dont_show_again(tr::now), [=] {
			DismissTopic(controller, id);
		}, &st::conversationMenuHide);
		menu->addItem(DestructiveItem(
			tr::lng_report(tr::now),
			[=] { ShowReportBox(controller, id); },
			&st::menuIconReport));
	}
	menu->popup(globalPosition);
}

void FillChatMenu(
		Ui::PopupMenu *menu,
		Window::SessionController *controller,
		const QString &chatId) {
	const auto session = &controller->session();
	const auto chat = session->data().chats().chat(chatId);
	if (!chat || !chat->isConversation || !chat->conversation) {
		return;
	}
	const auto &conversation = *chat->conversation;
	const auto id = conversation.id;
	const auto &state = conversation.state;
	const auto running = (state == u"open") || (state == u"active");
	const auto over = (state == u"ended")
		|| (state == u"expired")
		|| (state == u"closed")
		|| (state == u"purged");
	if (running) {
		menu->addItem(DestructiveItem(tr::lng_conv_end_conversation(tr::now), [=] {
			auto args = Ui::ConfirmBoxArgs();
			args.title = tr::lng_conv_end_conversation(tr::now);
			args.text = tr::lng_history_end_sure(tr::now);
			args.confirmText = tr::lng_conv_end_conversation(tr::now);
			args.destructive = true;
			args.confirmed = [=](std::function<void()> close) {
				session->data().conversations().end(id, controller, [=] {
					Q_EMIT Events()->topicHidden(id);
					controller->showToast(
						tr::lng_chat_conversation_has_ended(tr::now));
				}, [=](const Api::Error &error) {
					Support::ShowError(controller, error);
				});
				close();
			};
			controller->show(Ui::MakeConfirmBox(std::move(args)));
		}, &st::conversationMenuEnd));
	}
	if ((chat->anonymous || conversation.listener) && !over) {
		const auto name = Support::ChatTitle(session, *chat);
		menu->addItem(DestructiveItem(tr::lng_block_user(tr::now), [=] {
			auto args = Ui::ConfirmBoxArgs();
			args.title = tr::lng_block_user(tr::now);
			args.text = tr::lng_history_block_sure(tr::now, lt_name, name);
			args.confirmText = tr::lng_chat_block(tr::now);
			args.destructive = true;
			args.confirmed = [=](std::function<void()> close) {
				session->data().conversations().block(id, controller, [=] {
					controller->showToast(tr::lng_blocked_toast(tr::now));
				}, [=](const Api::Error &error) {
					Support::ShowError(controller, error);
				});
				close();
			};
			controller->show(Ui::MakeConfirmBox(std::move(args)));
		}, &st::menuIconBlock));
	}
}

void OpenChatOf(
		Window::SessionController *controller,
		const QString &conversationId) {
	controller->session().data().conversations().findChat(
		conversationId,
		controller,
		[=](const QString &chat) {
			if (chat.isEmpty()) {
				controller->showToast(
					tr::lng_conv_joined_will_appear_chats_shortly(tr::now));
			} else {
				controller->openRoute(Window::Routes::Chat(chat));
			}
		},
		controller->session().data().transport().connected()
			? kFindChatTimeout
			: kFindChatOfflineTimeout);
}

void OpenConversation(
		Window::SessionController *controller,
		const QString &id) {
	if (id.isEmpty()) {
		return;
	}
	auto &data = controller->session().data();
	const auto chatId = data.chats().chatOfConversation(id);
	if (!chatId.isEmpty()) {
		const auto chat = data.chats().chat(chatId);
		if (chat && chat->waitingForListener()) {
			controller->showMainSection(
				RadarFactory(id, chat->conversation),
				{ .tab = Window::Tab::Chats });
		} else {
			controller->openRoute(Window::Routes::Chat(chatId));
		}
		return;
	}
	// Not in my chats (yet): the view tells whose it is.
	data.conversations().get(id, controller, [=](
			const Data::Conversation &conversation) {
		const auto speaker = conversation.speaker.me
			|| (conversation.myRole == u"speaker");
		if (speaker && PhaseOf(
				conversation,
				QDateTime::currentMSecsSinceEpoch()) == RadarPhase::Waiting) {
			controller->showMainSection(
				RadarFactory(id, conversation),
				{ .tab = Window::Tab::Chats });
		} else if (speaker || conversation.myRole == u"listener") {
			controller->activateTab(Window::Tab::Chats);
			OpenChatOf(controller, id);
		} else {
			ShowJoinBox(controller, id);
		}
	}, [=](const Api::Error &error) {
		if (error.notFound() || error.code == u"conversation.not_open") {
			// The join box words it: «این گفتگو دیگر باز نیست».
			ShowJoinBox(controller, id);
		} else {
			Support::ShowError(controller, error, tr::lng_error_load(tr::now));
		}
	});
}

} // namespace Conversation
