#include "history/history_compose_controls.h"

#include "api/api_client.h"
#include "api/api_client_config.h"
#include "core/core_application.h"
#include "core/core_settings.h"
#include "data/data_chats.h"
#include "data/data_messages.h"
#include "data/data_outbox.h"
#include "data/data_session.h"
#include "data/data_stickers.h"
#include "history/history_bot_keyboard.h"
#include "history/history_boxes.h"
#include "history/history_logic.h"
#include "history/history_tabbed_panel.h"
#include "history/history_voice_recorder.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "storage/storage_database.h"
#include "support/chat_text.h"
#include "support/error_text.h"
#include "support/file_dialogs.h"
#include "support/media_image.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/tooltip.h"
#include "window/window_session_controller.h"
#include "styles/palette.h"
#include "styles/style_history.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>

namespace History {
namespace {

constexpr auto kPreviewDelay = 600;
constexpr auto kTypingInterval = qint64(3000);

[[nodiscard]] QHash<QString, QVector<Data::Message>> &PendingForwards() {
	static auto result = QHash<QString, QVector<Data::Message>>();
	return result;
}

[[nodiscard]] QVector<ComposeControls*> &LiveComposers() {
	static auto result = QVector<ComposeControls*>();
	return result;
}

[[nodiscard]] QString ForwardKey(Main::Session *session, const QString &chat) {
	return session->userId() + u'/' + chat;
}

[[nodiscard]] QVector<Data::Entity> SpoilersOf(const QVector<Data::Entity> &entities) {
	auto result = QVector<Data::Entity>();
	for (const auto &entity : entities) {
		if (entity.type == u"spoiler") {
			result.push_back(entity);
		}
	}
	return result;
}

[[nodiscard]] QString DurationText(qint64 ms) {
	const auto seconds = ms / 1000;
	return Lang::Num(u"%1:%2"_s
		.arg(seconds / 60)
		.arg(seconds % 60, 2, 10, QChar(u'0')));
}

} // namespace

void SetPendingForward(
		Main::Session *session,
		const QString &toChat,
		const QVector<Data::Message> &messages) {
	const auto key = ForwardKey(session, toChat);
	if (messages.isEmpty()) {
		PendingForwards().remove(key);
	} else {
		PendingForwards().insert(key, messages);
	}
	// A composer of that chat that is already open takes them at once.
	const auto composers = LiveComposers();
	for (const auto composer : composers) {
		QMetaObject::invokeMethod(composer, [composer] {
			composer->takePendingForward();
		}, Qt::QueuedConnection);
	}
}

ComposeControls::ComposeControls(
	QWidget *parent,
	Window::SessionController *controller,
	const QString &chatId,
	QWidget *panelParent)
: RpWidget(parent)
, _controller(controller)
, _session(&controller->session())
, _chatId(chatId)
, _field(Ui::CreateChild<Ui::InputField>(
	this,
	st::historyComposeField,
	Ui::InputField::Mode::MultiLine,
	tr::lng_chat_message_placeholder(tr::now)))
, _attach(Ui::CreateChild<Ui::IconButton>(this, st::historyAttachButton))
, _emoji(Ui::CreateChild<Ui::IconButton>(this, st::historyEmojiButton))
, _send(Ui::CreateChild<Ui::IconButton>(this, st::historySendButton))
, _mic(Ui::CreateChild<Ui::IconButton>(this, st::historyMicButton))
, _barClose(Ui::CreateChild<Ui::IconButton>(this, st::historyComposeBarClose))
, _previewClose(Ui::CreateChild<Ui::IconButton>(this, st::historyComposeBarClose))
, _recordCancel(Ui::CreateChild<Ui::IconButton>(this, st::historyRecordCancel))
, _panel(Ui::CreateChild<TabbedPanel>(panelParent, _session))
, _keyboard(Ui::CreateChild<BotKeyboard>(this))
, _previewTimer([this] { checkPreview(); })
, _draftTimer([this] {
	if (_draft) {
		_draft->timeout(Ui::Now());
	}
}) {
	if (const auto chat = _session->data().chats().chat(_chatId)) {
		_conversation = chat->isConversation;
	}
	LiveComposers().push_back(this);
	_panel->hide();
	_barClose->hide();
	_previewClose->hide();
	_recordCancel->hide();
	setupField();
	setupButtons();
	setupDraft();
	takePendingForward();
	_keyboard->hide();
	_keyboard->setPressedCallback([this](const QString &text) {
		// Like typing the text and pressing send; a draft stays in the field.
		if (_keyboardOneTime) {
			_keyboardUsedSeq = ReplyKeyboardSeq(
				_session->data().messages().history(_chatId).items);
		}
		_session->data().outbox().sendText(_chatId, text);
		refreshBotKeyboard();
		Q_EMIT sent();
	});
	connect(
		&_session->data().messages(),
		&Data::Messages::historyChanged,
		this,
		[this](const QString &chat) {
			if (chat == _chatId) {
				refreshBotKeyboard();
			}
		});
	refreshBotKeyboard();
	updateControls();
}

ComposeControls::~ComposeControls() {
	LiveComposers().removeAll(this);
	if (_recorder) {
		_recorder->stop(false);
	}
	flushDraft();
}

void ComposeControls::setupField() {
	const auto raw = _field->rawTextEdit();
	raw->installEventFilter(this);
	// Dropped files belong to the whole section, not to the editor.
	raw->setAcceptDrops(false);
	raw->viewport()->setAcceptDrops(false);
	_field->setSubmitSettings(Core::App().settings().sendByCtrlEnter()
		? Ui::InputField::SubmitSettings::CtrlEnter
		: Ui::InputField::SubmitSettings::Enter);
	connect(&Core::App().settings(), &Core::Settings::changed, this, [this] {
		_field->setSubmitSettings(Core::App().settings().sendByCtrlEnter()
			? Ui::InputField::SubmitSettings::CtrlEnter
			: Ui::InputField::SubmitSettings::Enter);
	});
	connect(
		raw->document(),
		&QTextDocument::contentsChange,
		this,
		[this](int position, int removed, int added) {
			Q_UNUSED(removed);
			Q_UNUSED(added);
			_changeHint = position;
		});
	connect(_field, &Ui::InputField::changed, this, [this] { fieldChanged(); });
	connect(_field, &Ui::InputField::submitted, this, [this] { submit(); });
	connect(_field, &Ui::RpWidget::heightChanged, this, [this] {
		resizeToWidth(width());
	});
	_field->setContextMenuHook([this](Ui::PopupMenu *menu) {
		fillFieldMenu(menu);
	});
}

void ComposeControls::setupButtons() {
	_attach->setClickedCallback([this] { showAttachMenu(); });
	_emoji->setClickedCallback([this] { _panel->toggle(_emoji); });
	_send->setClickedCallback([this] { submit(); });
	_mic->setClickedCallback([this] { startRecording(); });
	_barClose->setClickedCallback([this] { cancelMode(); });
	_previewClose->setClickedCallback([this] { dismissPreview(); });
	_recordCancel->setClickedCallback([this] { stopRecording(false); });
	Ui::InstallTooltip(_attach, [] { return tr::lng_chat_attach(tr::now); });
	Ui::InstallTooltip(_emoji, [] { return tr::lng_history_tab_emoji(tr::now); });
	Ui::InstallTooltip(_send, [] { return tr::lng_send(tr::now); });
	Ui::InstallTooltip(_mic, [] { return tr::lng_history_record_voice(tr::now); });
	Ui::InstallTooltip(_recordCancel, [] { return tr::lng_cancel(tr::now); });

	connect(_panel, &TabbedPanel::emojiChosen, this, [this](const QString &emoji) {
		_field->insertText(emoji);
	});
	connect(_panel, &TabbedPanel::stickerChosen, this, [this](const Data::Sticker &sticker) {
		_panel->hideAnimated();
		sendSticker(sticker);
	});
	connect(_panel, &TabbedPanel::panelShownChanged, this, [this](bool shown) {
		_emoji->setIconOverride(
			shown ? &st::historyEmojiButtonActiveIcon : nullptr,
			shown ? &st::historyEmojiButtonActiveIcon : nullptr);
		if (!shown) {
			_field->setFocusFast();
		}
	});
}

QString ComposeControls::localDraftKey() const {
	return u"history.draft."_s + _chatId;
}

void ComposeControls::setupDraft() {
	auto delegate = DraftSaver::Delegate();
	delegate.save = [this](const Data::Draft &draft) {
		if (_conversation) {
			_session->local().setValue(localDraftKey(), draft.toJson());
		} else {
			_session->data().chats().saveDraft(_chatId, draft);
		}
	};
	delegate.clear = [this] {
		if (_conversation) {
			_session->local().setValue(localDraftKey(), QJsonValue());
		} else {
			_session->data().chats().clearDraft(_chatId);
		}
	};
	delegate.arm = [this](qint64 delay) {
		_draftTimer.callOnce(int(delay));
	};
	delegate.disarm = [this] {
		_draftTimer.cancel();
	};
	_draft = std::make_unique<DraftSaver>(std::move(delegate));

	const auto apply = [this](const Data::Draft &draft) {
		_draft->reset(draft);
		if (draft.empty()) {
			return;
		}
		setFieldText(draft.text, SpoilersOf(draft.entities));
		if (draft.replyToSeq > 0) {
			const auto target = _session->data().messages().find(
				_chatId,
				draft.replyToSeq);
			if (target) {
				_target = *target;
				_quote = std::nullopt;
				setMode(Mode::Reply);
			}
		}
	};
	if (_conversation) {
		_session->local().value(localDraftKey(), this, [=, this](QJsonValue value) {
			_draftReady = true;
			if (value.isObject() && _text.isEmpty() && _mode == Mode::None) {
				apply(Data::Draft::FromJson(value.toObject()));
			}
		});
	} else {
		_draftReady = true;
		const auto chat = _session->data().chats().chat(_chatId);
		if (chat && chat->draft) {
			apply(*chat->draft);
		}
		connect(
			&_session->data().chats(),
			&Data::Chats::chatChanged,
			this,
			[this](const QString &id) {
				if (id == _chatId) {
					applyRemoteDraft();
				}
			});
	}
}

void ComposeControls::applyRemoteDraft() {
	const auto chat = _session->data().chats().chat(_chatId);
	if (!chat || _mode == Mode::Edit || recording()) {
		return;
	}
	if (!_draft->acceptsRemote(chat->draft, fieldFocused())) {
		return;
	}
	const auto draft = chat->draft.value_or(Data::Draft());
	_draft->reset(draft);
	setFieldText(draft.text, SpoilersOf(draft.entities));
}

void ComposeControls::flushDraft() {
	if (_draft && _draftReady) {
		_draft->flush();
	}
}

Data::Draft ComposeControls::currentDraft() const {
	auto result = Data::Draft();
	result.text = _text;
	result.entities = _spoilers;
	result.replyToSeq = (_mode == Mode::Reply) ? _target.seq : 0;
	return result;
}

void ComposeControls::setInnerFocus() {
	takePendingForward();
	if (!recording()) {
		_field->setFocusFast();
	}
}

void ComposeControls::typeFromOutside(QKeyEvent *e) {
	if (recording()) {
		return;
	}
	_field->setFocusFast();
	QCoreApplication::sendEvent(_field->rawTextEdit(), e);
}

bool ComposeControls::fieldFocused() const {
	return _field->hasFocus();
}

// ---- text and spoilers

void ComposeControls::setFieldText(
		const QString &text,
		const QVector<Data::Entity> &entities) {
	_settingText = true;
	_spoilers = NormalizeEntities(text, entities);
	_text = text;
	_field->setText(text);
	_text = _field->text();
	_settingText = false;
	_changeHint = -1;
	applySpoilerMarks();
	updateControls();
	_previewTimer.callOnce(kPreviewDelay);
}

void ComposeControls::fieldChanged() {
	const auto now = _field->text();
	if (!_settingText) {
		_spoilers = AdjustEntities(_spoilers, _text, now, _changeHint);
	}
	const auto was = _text;
	_text = now;
	_changeHint = -1;
	applySpoilerMarks();
	updateControls();
	if (_settingText || was == now) {
		return;
	}
	if (_mode != Mode::Edit) {
		_draft->changed(currentDraft(), Ui::Now());
		if (!now.trimmed().isEmpty()) {
			typing();
		}
	}
	_previewTimer.callOnce(kPreviewDelay);
}

void ComposeControls::applySpoilerMarks() {
	const auto raw = _field->rawTextEdit();
	auto selections = QList<QTextEdit::ExtraSelection>();
	const auto length = int(_text.size());
	for (const auto &entity : std::as_const(_spoilers)) {
		const auto from = std::clamp(entity.offset, 0, length);
		const auto till = std::clamp(entity.offset + entity.length, from, length);
		if (till <= from) {
			continue;
		}
		auto selection = QTextEdit::ExtraSelection();
		selection.cursor = QTextCursor(raw->document());
		selection.cursor.setPosition(from);
		selection.cursor.setPosition(till, QTextCursor::KeepAnchor);
		selection.format.setBackground(st::historyComposeSpoilerBg->c);
		selections.push_back(selection);
	}
	raw->setExtraSelections(selections);
}

void ComposeControls::toggleSpoiler() {
	const auto cursor = _field->rawTextEdit()->textCursor();
	if (!cursor.hasSelection()) {
		return;
	}
	_spoilers = ToggleSpoiler(
		_text,
		_spoilers,
		cursor.selectionStart(),
		cursor.selectionEnd());
	applySpoilerMarks();
	if (_mode != Mode::Edit) {
		_draft->changed(currentDraft(), Ui::Now());
	}
	_previewTimer.callOnce(kPreviewDelay);
}

void ComposeControls::fillFieldMenu(Ui::PopupMenu *menu) {
	const auto cursor = _field->rawTextEdit()->textCursor();
	const auto has = cursor.hasSelection();
	menu->addSeparator();
	auto item = Ui::MenuItem();
	item.text = tr::lng_history_spoiler(tr::now);
	item.icon = &st::menuIconSpoiler;
	item.disabled = !has;
	item.checked = has
		&& RangeIsSpoiler(_spoilers, cursor.selectionStart(), cursor.selectionEnd());
	item.shortcut = QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P).toString(
		QKeySequence::NativeText);
	item.callback = [this] { toggleSpoiler(); };
	menu->addItem(std::move(item));
}

bool ComposeControls::eventFilter(QObject *object, QEvent *event) {
	if (object == _field->rawTextEdit() && event->type() == QEvent::KeyPress) {
		if (fieldKeyPress(static_cast<QKeyEvent*>(event))) {
			return true;
		}
	}
	return RpWidget::eventFilter(object, event);
}

bool ComposeControls::fieldKeyPress(QKeyEvent *e) {
	const auto key = e->key();
	const auto modifiers = e->modifiers()
		& (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
	if (key == Qt::Key_P && modifiers == (Qt::ControlModifier | Qt::ShiftModifier)) {
		toggleSpoiler();
		return true;
	} else if (key == Qt::Key_Up && !modifiers && _text.isEmpty() && _mode == Mode::None) {
		if (_lastEditable) {
			if (const auto message = _lastEditable()) {
				editMessage(*message);
				return true;
			}
		}
		return false;
	} else if ((key == Qt::Key_PageUp || key == Qt::Key_PageDown) && !modifiers) {
		Q_EMIT scrollRequested(key == Qt::Key_PageUp);
		return true;
	} else if (e->matches(QKeySequence::Paste)) {
		return pasteFromClipboard();
	} else if (e->matches(QKeySequence::Copy)) {
		if (!_field->rawTextEdit()->textCursor().hasSelection()) {
			Q_EMIT copyRequested();
			return true;
		}
	}
	return false;
}

bool ComposeControls::pasteFromClipboard() {
	if (_mode == Mode::Edit) {
		return false;
	}
	const auto data = Support::ReadMimeData(QGuiApplication::clipboard()->mimeData());
	if (!data.files.isEmpty()) {
		attachFiles(data.files);
		return true;
	} else if (!data.image.isNull() && data.text.isEmpty()) {
		attachImage(data.image);
		return true;
	}
	return false;
}

// ---- modes

void ComposeControls::setMode(Mode mode) {
	_mode = mode;
	refreshBarThumb();
	updateControls();
	resizeToWidth(width());
	update();
}

void ComposeControls::replyTo(
		const Data::Message &message,
		const std::optional<Data::Quote> &quote) {
	if (message.seq <= 0 || recording()) {
		return;
	}
	if (_mode == Mode::Edit) {
		cancelMode();
	}
	// A reply replaces what waited to be forwarded.
	if (_mode == Mode::Forward) {
		_forwards.clear();
	}
	_target = message;
	_quote = quote;
	setMode(Mode::Reply);
	_draft->changed(currentDraft(), Ui::Now());
	_field->setFocusFast();
}

void ComposeControls::editMessage(const Data::Message &message) {
	if (message.seq <= 0 || recording()) {
		return;
	}
	if (_mode != Mode::Edit) {
		// What was being typed comes back when the edit is over.
		_stashed = currentDraft();
		flushDraft();
	}
	_forwards.clear();
	_draft->setSuspended(true);
	_target = message;
	_quote = std::nullopt;
	_editSaving = false;
	_previewDismissed = message.linkPreview ? QString() : u"*"_s;
	setFieldText(message.text, SpoilersOf(message.entities));
	setMode(Mode::Edit);
	_field->setFocusFast();
	_field->setCursorToEnd();
}

void ComposeControls::cancelMode() {
	const auto was = _mode;
	if (was == Mode::None) {
		return;
	}
	_mode = Mode::None;
	_forwards.clear();
	_quote = std::nullopt;
	_editSaving = false;
	if (was == Mode::Edit) {
		_draft->setSuspended(false);
		_previewDismissed = QString();
		const auto stashed = std::exchange(_stashed, Data::Draft());
		setFieldText(stashed.text, stashed.entities);
		if (stashed.replyToSeq > 0) {
			const auto target = _session->data().messages().find(
				_chatId,
				stashed.replyToSeq);
			if (target) {
				_target = *target;
				_mode = Mode::Reply;
			}
		}
	} else if (was == Mode::Reply) {
		_draft->changed(currentDraft(), Ui::Now());
	}
	setMode(_mode);
	_field->setFocusFast();
}

bool ComposeControls::cancelCurrent() {
	if (recording()) {
		stopRecording(false);
		return true;
	} else if (_panel->shown()) {
		_panel->hideAnimated();
		return true;
	} else if (_mode != Mode::None) {
		cancelMode();
		return true;
	}
	return false;
}

void ComposeControls::takePendingForward() {
	const auto key = ForwardKey(_session, _chatId);
	const auto i = PendingForwards().find(key);
	if (i == PendingForwards().end()) {
		return;
	}
	const auto messages = *i;
	PendingForwards().erase(i);
	if (messages.isEmpty() || recording()) {
		return;
	}
	if (_mode == Mode::Edit) {
		cancelMode();
	}
	_forwards = messages;
	_quote = std::nullopt;
	_target = messages.front();
	setMode(Mode::Forward);
}

void ComposeControls::refreshBarThumb() {
	QObject::disconnect(_barThumbConnection);
	_barThumb = nullptr;
	_barThumbPrepared = QImage();
	_barThumbKey = 0;
	const auto single = (_mode == Mode::Reply)
		|| (_mode == Mode::Edit)
		|| (_mode == Mode::Forward && _forwards.size() == 1);
	if (!single || _target.mediaSpoiler) {
		return;
	}
	_barThumb = Support::MakeMessageImageSource(
		_session,
		_target,
		Data::MediaVariant::Thumb);
	if (_barThumb) {
		_barThumbConnection = connect(
			_barThumb.get(),
			&Ui::ImageSource::updated,
			this,
			[this] { update(); });
	}
}

// ---- sending

void ComposeControls::typing() {
	const auto now = Ui::Now();
	if (now - _typingSentAt < kTypingInterval) {
		return;
	}
	_typingSentAt = now;
	_session->data().chats().sendTyping(_chatId);
}

void ComposeControls::submit() {
	if (recording()) {
		stopRecording(true);
	} else if (_mode == Mode::Edit) {
		saveEdit();
	} else {
		send();
	}
}

void ComposeControls::send() {
	const auto config = _session->data().clientConfig();
	const auto prepared = PrepareForSend(
		_text,
		_spoilers,
		config ? config->maxEntities() : 0);
	if (prepared.empty() && _forwards.isEmpty()) {
		return;
	}
	// Before the outbox touches the chat row: the stored draft is cleared
	// on the server too, a pending save never fires.
	_draft->sent();
	auto &outbox = _session->data().outbox();
	if (!prepared.empty()) {
		const auto parts = SplitForSend(
			prepared,
			config ? config->maxMessageLength() : 0);
		auto first = true;
		for (const auto &part : parts) {
			auto options = Data::SendTextOptions();
			options.entities = part.entities;
			if (first) {
				if (_mode == Mode::Reply) {
					options.replyToSeq = _target.seq;
					options.quote = _quote;
				}
				if (_preview.ready && !_preview.data.url.isEmpty()) {
					options.linkPreview = _preview.data.url;
				}
				first = false;
			}
			outbox.sendText(_chatId, part.text, options);
		}
	}
	sendForwards();

	_mode = Mode::None;
	_quote = std::nullopt;
	_forwards.clear();
	_preview = Preview();
	_previewDismissed = QString();
	++_previewRequest;
	_typingSentAt = 0;
	setFieldText(QString(), {});
	_previewTimer.cancel();
	setMode(Mode::None);
	Q_EMIT sent();
}

void ComposeControls::sendForwards() {
	auto &outbox = _session->data().outbox();
	for (const auto &message : std::as_const(_forwards)) {
		outbox.forward(_chatId, message);
	}
}

void ComposeControls::saveEdit() {
	if (_editSaving) {
		return;
	}
	const auto config = _session->data().clientConfig();
	const auto prepared = PrepareForSend(
		_text,
		_spoilers,
		config ? config->maxEntities() : 0);
	const auto limit = config
		? ((_target.kind == Data::MessageKind::Text)
			? config->maxMessageLength()
			: config->maxCaptionLength())
		: 0;
	if ((prepared.empty() && _target.kind == Data::MessageKind::Text)
		|| (limit > 0 && prepared.text.size() > limit)) {
		_field->showError();
		return;
	}
	const auto previewUrl = (_preview.ready && !_preview.data.url.isEmpty())
		? _preview.data.url
		: QString();
	const auto hadPreview = _target.linkPreview
		? _target.linkPreview->url
		: QString();
	const auto sameSpoilers = [&] {
		const auto was = NormalizeEntities(_target.text, SpoilersOf(_target.entities));
		if (was.size() != prepared.entities.size()) {
			return false;
		}
		for (auto i = 0; i != int(was.size()); ++i) {
			if (was[i].offset != prepared.entities[i].offset
				|| was[i].length != prepared.entities[i].length) {
				return false;
			}
		}
		return true;
	}();
	if (prepared.text == _target.text && sameSpoilers && previewUrl == hadPreview) {
		cancelMode();
		return;
	}
	_editSaving = true;
	updateControls();
	const auto done = [this] {
		_editSaving = false;
		cancelMode();
	};
	const auto fail = [this](const Api::Error &error) {
		_editSaving = false;
		updateControls();
		Support::ShowError(_controller, error);
	};
	auto &messages = _session->data().messages();
	if (config && config->linkPreviewEnabled()) {
		messages.editWithPreview(
			_chatId,
			_target.seq,
			prepared.text,
			prepared.entities,
			previewUrl,
			this,
			done,
			fail);
	} else {
		messages.edit(
			_chatId,
			_target.seq,
			prepared.text,
			prepared.entities,
			this,
			done,
			fail);
	}
}

void ComposeControls::sendSticker(const Data::Sticker &sticker) {
	if (_mode == Mode::Edit) {
		return;
	}
	const auto replyTo = (_mode == Mode::Reply) ? _target.seq : qint64(0);
	_session->data().outbox().sendSticker(_chatId, sticker.id, replyTo);
	_session->data().stickers().noteUsed(sticker);
	if (_mode == Mode::Reply) {
		_mode = Mode::None;
		_quote = std::nullopt;
		_draft->changed(currentDraft(), Ui::Now());
		setMode(Mode::None);
	}
	Q_EMIT sent();
}

void ComposeControls::showAttachMenu() {
	if (_attachMenu) {
		_attachMenu->hideMenu();
		return;
	}
	_attachMenu = new Ui::PopupMenu(this);
	_attachMenu->addAction(
		tr::lng_chat_photo_video(tr::now),
		[this] { pickFiles(true); },
		&st::menuIconImage);
	_attachMenu->addAction(
		tr::lng_media_file(tr::now),
		[this] { pickFiles(false); },
		&st::menuIconFile);
	_attachMenu->addAction(
		tr::lng_media_location(tr::now),
		[this] {
			const auto chat = _chatId;
			const auto session = _session;
			const auto replyTo = (_mode == Mode::Reply) ? _target.seq : qint64(0);
			_controller->show(LocationBox(base::guard(this, [=, this](
					const Data::Location &location) {
				session->data().outbox().sendLocation(chat, location, replyTo);
				if (_mode == Mode::Reply) {
					cancelMode();
				}
				Q_EMIT sent();
			})));
		},
		&st::menuIconLocation);
	_attachMenu->popupAnchored(_attach, style::al_left);
}

void ComposeControls::pickFiles(bool media) {
	auto args = Support::PickArgs();
	args.kind = media ? Support::PickKind::Media : Support::PickKind::Any;
	args.multiple = true;
	Support::PickFiles(window(), args, base::guard(this, [this, media](
			const QStringList &paths) {
		if (paths.isEmpty()) {
			return;
		}
		Q_UNUSED(media);
		attachFiles(paths);
	}));
}

void ComposeControls::attachImage(const QImage &image) {
	if (image.isNull()) {
		return;
	}
	Support::SaveImageToTemp(image, this, [this](const QString &path) {
		if (path.isEmpty()) {
			showToast(tr::lng_chat_file_cant_be_sent(tr::now));
		} else {
			attachFiles({ path });
		}
	});
}

void ComposeControls::attachFiles(const QStringList &paths) {
	if (paths.isEmpty() || _mode == Mode::Edit || recording()) {
		return;
	}
	const auto config = _session->data().clientConfig();
	const auto limit = config ? config->maxUploadBytes() : qint64(0);
	auto good = QStringList();
	auto tooLarge = false;
	for (const auto &path : paths) {
		const auto info = QFileInfo(path);
		if (!info.isFile() || !info.isReadable() || info.size() <= 0) {
			continue;
		} else if (limit > 0 && info.size() > limit) {
			tooLarge = true;
			continue;
		}
		good.push_back(info.absoluteFilePath());
	}
	if (good.isEmpty()) {
		showToast(tooLarge
			? tr::lng_chat_file_too_large(tr::now)
			: tr::lng_chat_file_cant_be_sent(tr::now));
		return;
	} else if (tooLarge) {
		showToast(tr::lng_chat_file_too_large(tr::now));
	}
	// What is typed becomes the caption (and comes back if the box is closed).
	const auto caption = _text.trimmed();
	_controller->show(SendFilesBox(_session, good, caption, base::guard(this, [this](
			const FilesToSend &files) {
		_draft->sent();
		auto &outbox = _session->data().outbox();
		const auto replyTo = (_mode == Mode::Reply) ? _target.seq : qint64(0);
		auto captionLeft = !files.caption.isEmpty();
		auto first = true;
		for (const auto &path : files.paths) {
			const auto visual = Support::IsImageFile(path) || Support::IsVideoFile(path);
			auto options = Data::SendFileOptions();
			options.replyToSeq = first ? replyTo : 0;
			options.spoiler = files.spoiler && visual && !files.asFiles;
			options.fileName = QFileInfo(path).fileName();
			if (files.asFiles) {
				options.kind = Data::UploadKind::File;
			}
			// A picture carries the caption; a file keeps its name as its
			// text, so its caption follows as a message of its own.
			if (captionLeft && visual && !files.asFiles && files.paths.size() == 1) {
				options.caption = files.caption;
				options.entities = files.entities;
				captionLeft = false;
			}
			outbox.sendFile(_chatId, path, options);
			first = false;
		}
		if (captionLeft) {
			auto options = Data::SendTextOptions();
			options.entities = files.entities;
			outbox.sendText(_chatId, files.caption, options);
		}
		_mode = Mode::None;
		_quote = std::nullopt;
		setFieldText(QString(), {});
		setMode(Mode::None);
		Q_EMIT sent();
	})));
}

// ---- link preview

void ComposeControls::checkPreview() {
	const auto config = _session->data().clientConfig();
	if (!config || !config->linkPreviewEnabled() || _mode == Mode::Forward) {
		return;
	}
	const auto url = FirstLinkForPreview(_text, _spoilers);
	if (url.isEmpty() || url == _previewDismissed || _previewDismissed == u"*") {
		if (url.isEmpty() && _previewDismissed != u"*") {
			_previewDismissed = QString();
		}
		if (!_preview.url.isEmpty()) {
			_preview = Preview();
			++_previewRequest;
			updateControls();
			resizeToWidth(width());
			update();
		}
		return;
	} else if (url == _preview.url) {
		return;
	}
	requestPreview(url);
}

void ComposeControls::requestPreview(const QString &url) {
	const auto had = _preview.ready;
	_preview = Preview();
	_preview.url = url;
	_preview.loading = true;
	const auto request = ++_previewRequest;
	if (had) {
		updateControls();
		resizeToWidth(width());
		update();
	}
	_session->api().request(Api::Post, u"/v1/link-preview"_s)
		.body(QJsonObject{ { u"url"_s, url } })
		.noIdempotencyKey()
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			if (request != _previewRequest) {
				return;
			}
			auto data = Data::LinkPreview::FromJson(result);
			_preview.loading = false;
			if (data.url.isEmpty()) {
				data.url = url;
			}
			_preview.ready = !data.title.isEmpty()
				|| !data.description.isEmpty()
				|| !data.siteName.isEmpty();
			_preview.data = data;
			updateControls();
			resizeToWidth(width());
			update();
		})
		.fail([=, this](const Api::Error &) {
			if (request != _previewRequest) {
				return;
			}
			// No card for this address: nothing to show, nothing to send.
			_preview.loading = false;
			_preview.ready = false;
			updateControls();
			resizeToWidth(width());
			update();
		})
		.send();
}

void ComposeControls::dismissPreview() {
	_previewDismissed = _preview.url;
	_preview = Preview();
	++_previewRequest;
	updateControls();
	resizeToWidth(width());
	update();
	_field->setFocusFast();
}

// ---- voice

bool ComposeControls::recording() const {
	return _recorder && _recorder->recording();
}

void ComposeControls::startRecording() {
	if (recording() || _mode == Mode::Edit) {
		return;
	}
	if (!VoiceRecorder::Available()) {
		showToast(tr::lng_history_no_microphone(tr::now));
		return;
	}
	if (!_recorder) {
		_recorder = std::make_unique<VoiceRecorder>();
		connect(_recorder.get(), &VoiceRecorder::durationChanged, this, [this](qint64 ms) {
			_recordedMs = ms;
			const auto config = _session->data().clientConfig();
			const auto limit = config ? config->maxVoiceDuration() * 1000 : qint64(0);
			if (limit > 0 && ms >= limit) {
				stopRecording(true);
			}
			update();
		});
		connect(_recorder.get(), &VoiceRecorder::finished, this, [this](
				const QString &path,
				qint64 durationMs) {
			_recordPulse.stop();
			auto options = Data::SendFileOptions();
			options.asVoice = true;
			options.durationMs = durationMs;
			if (_mode == Mode::Reply) {
				options.replyToSeq = _target.seq;
			}
			_session->data().outbox().sendFile(_chatId, path, options);
			if (_mode == Mode::Reply) {
				cancelMode();
			}
			updateControls();
			Q_EMIT sent();
		});
		connect(_recorder.get(), &VoiceRecorder::failed, this, [this] {
			_recordPulse.stop();
			_session->data().chats().sendTyping(_chatId, u"cancel"_s);
			updateControls();
			showToast(tr::lng_chat_microphone_access_was_denied(tr::now));
		});
	}
	_recordedMs = 0;
	if (!_recorder->start()) {
		showToast(tr::lng_chat_microphone_access_was_denied(tr::now));
		return;
	}
	_panel->hideFast();
	_session->data().chats().sendTyping(_chatId, u"recording_voice"_s);
	_recordPulse.init([this](qint64) {
		update();
		return true;
	});
	_recordPulse.start();
	updateControls();
}

void ComposeControls::stopRecording(bool send) {
	if (!recording()) {
		return;
	}
	_recorder->stop(send);
	_recordPulse.stop();
	if (!send) {
		_session->data().chats().sendTyping(_chatId, u"cancel"_s);
	}
	updateControls();
	_field->setFocusFast();
}

void ComposeControls::showToast(const QString &text) {
	// Above the composer, not over what is being typed.
	auto config = Ui::Toast::Config();
	config.text = text;
	config.bottomSkip = height();
	_controller->showToast(std::move(config));
}

// ---- the bot's reply keyboard

void ComposeControls::refreshBotKeyboard() {
	const auto chat = _session->data().chats().chat(_chatId);
	auto markup = std::optional<Data::ReplyMarkup>();
	if (chat && chat->isBot) {
		markup = CurrentReplyKeyboard(
			_session->data().messages().history(_chatId).items,
			_keyboardUsedSeq);
	}
	const auto was = keyboardHeight();
	_keyboardOneTime = markup && markup->oneTime;
	_keyboard->setRows(markup ? markup->keyboard : QVector<QStringList>());
	_field->setPlaceholder((markup && !markup->placeholder.isEmpty())
		? markup->placeholder
		: tr::lng_chat_message_placeholder(tr::now));
	const auto shown = !_keyboard->empty()
		&& !recording()
		&& (_mode != Mode::Edit);
	if (_keyboard->isHidden() == shown || was != keyboardHeight()) {
		_keyboard->setVisible(shown);
		if (width() > 0) {
			resizeToWidth(width());
		}
	}
}

int ComposeControls::keyboardHeight() const {
	return (_keyboard->empty() || recording() || _mode == Mode::Edit)
		? 0
		: _keyboard->height();
}

// ---- layout and painting

void ComposeControls::updateControls() {
	const auto rec = recording();
	const auto hasText = !_text.trimmed().isEmpty();
	const auto edit = (_mode == Mode::Edit);
	const auto canSend = hasText || edit || !_forwards.isEmpty() || rec;
	_field->setVisible(!rec);
	_attach->setVisible(!rec && !edit);
	_emoji->setVisible(!rec);
	_recordCancel->setVisible(rec);
	_send->setVisible(canSend);
	_mic->setVisible(!canSend);
	_send->setIconOverride(
		edit ? &st::historySaveEditIcon : nullptr,
		edit ? &st::historySaveEditIconOver : nullptr);
	_send->setDisabled(_editSaving);
	_barClose->setVisible(_mode != Mode::None && !rec);
	_previewClose->setVisible(previewHeight() > 0);
	const auto keyboard = !_keyboard->empty() && !rec && !edit;
	if (_keyboard->isHidden() == keyboard) {
		_keyboard->setVisible(keyboard);
		if (width() > 0) {
			resizeToWidth(width());
		}
	}
	updateFieldGeometry();
	update();
}

int ComposeControls::barHeight() const {
	return (_mode != Mode::None && !recording()) ? st::historyComposeBarHeight : 0;
}

int ComposeControls::previewHeight() const {
	return (_preview.ready && !recording() && _mode != Mode::Forward)
		? st::historyComposeBarHeight
		: 0;
}

QRect ComposeControls::contentRect() const {
	const auto available = width();
	const auto inner = std::min(available, int(st::historyComposeMaxWidth));
	return QRect((available - inner) / 2, 0, inner, height());
}

int ComposeControls::resizeGetHeight(int newWidth) {
	const auto &padding = st::historyComposePadding;
	const auto inner = std::min(newWidth, int(st::historyComposeMaxWidth));
	const auto leading = (_mode == Mode::Edit) ? 0 : _attach->width();
	const auto fieldWidth = inner
		- padding.left()
		- padding.right()
		- leading
		- _emoji->width()
		- _send->width();
	_field->resizeToWidth(std::max(fieldWidth, 40));
	const auto row = recording()
		? int(st::historyComposeField.heightMin)
		: _field->height();
	if (!_keyboard->empty()) {
		_keyboard->resizeToWidth(inner);
	}
	const auto result = st::lineWidth
		+ barHeight()
		+ previewHeight()
		+ padding.top()
		+ row
		+ padding.bottom()
		+ keyboardHeight();
	// Children of a section that is still sliding in get no resize event:
	// place them for the size that is about to be set.
	layoutControls(newWidth, result);
	return result;
}

void ComposeControls::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateFieldGeometry();
	updatePanelPosition();
}

void ComposeControls::updatePanelPosition() {
	_panel->updatePosition();
}

void ComposeControls::updateFieldGeometry() {
	layoutControls(width(), height());
}

void ComposeControls::layoutControls(int width, int height) {
	if (width <= 0 || height <= 0) {
		return;
	}
	const auto &padding = st::historyComposePadding;
	const auto inner = std::min(width, int(st::historyComposeMaxWidth));
	const auto side = (width - inner) / 2;
	const auto barsBottom = st::lineWidth + barHeight() + previewHeight();
	const auto rowTop = barsBottom + padding.top();
	const auto keyboard = keyboardHeight();
	const auto rowHeight = height - rowTop - padding.bottom() - keyboard;
	const auto buttonTop = rowTop + rowHeight - _send->height();
	if (keyboard > 0) {
		_keyboard->move(side, height - keyboard);
	}
	const auto edit = (_mode == Mode::Edit);

	auto left = side + padding.left();
	if (recording()) {
		_recordCancel->moveToLeft(left, buttonTop, width);
	} else if (!edit) {
		_attach->moveToLeft(left, buttonTop, width);
		left += _attach->width();
	}
	auto right = side + padding.right();
	_send->moveToRight(right, buttonTop, width);
	_mic->moveToRight(right, buttonTop, width);
	right += _send->width();
	_emoji->moveToRight(right, buttonTop, width);
	right += _emoji->width();
	_field->moveToLeft(left, rowTop, width);

	const auto closeRight = side + st::historyComposeBarPadding.right();
	auto top = st::lineWidth;
	if (barHeight() > 0) {
		_barClose->moveToRight(
			closeRight,
			top + (st::historyComposeBarHeight - _barClose->height()) / 2,
			width);
		top += barHeight();
	}
	if (previewHeight() > 0) {
		_previewClose->moveToRight(
			closeRight,
			top + (st::historyComposeBarHeight - _previewClose->height()) / 2,
			width);
	}
}

void ComposeControls::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto barTop = st::lineWidth;
	if (barHeight() > 0
		&& e->pos().y() >= barTop
		&& e->pos().y() < barTop + barHeight()
		&& (_mode == Mode::Reply || _mode == Mode::Edit)
		&& _target.seq > 0) {
		Q_EMIT jumpRequested(_target.seq);
		return;
	}
	if (!recording()) {
		_field->setFocusFast();
	}
}

void ComposeControls::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), st::historyComposeBg);
	p.fillRect(0, 0, width(), st::lineWidth, st::historyComposeLineFg);
	const auto content = contentRect();
	auto top = st::lineWidth;
	if (barHeight() > 0) {
		paintBar(p, QRect(content.x(), top, content.width(), barHeight()));
		top += barHeight();
	}
	if (previewHeight() > 0) {
		paintPreview(p, QRect(content.x(), top, content.width(), previewHeight()));
		top += previewHeight();
	}
	if (recording()) {
		const auto &padding = st::historyComposePadding;
		paintRecording(p, QRect(
			content.x(),
			top + padding.top(),
			content.width(),
			height() - top - padding.top() - padding.bottom() - keyboardHeight()));
	}
}

void ComposeControls::paintBar(Painter &p, QRect rect) {
	const auto &padding = st::historyComposeBarPadding;
	const auto &icon = (_mode == Mode::Edit)
		? st::historyComposeEditIcon
		: (_mode == Mode::Forward)
		? st::historyForwardedIcon
		: _quote
		? st::historyComposeQuoteIcon
		: st::historyComposeReplyIcon;
	auto left = rect.x() + padding.left();
	icon.paint(p, left, rect.y() + (rect.height() - icon.height()) / 2, width());
	left += icon.width() + st::historyComposeBarIconSkip;

	// The accent line.
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::historyReplyBarFg);
		p.drawRoundedRect(
			QRectF(myrtlrect(
				left,
				rect.y() + (rect.height() - st::historyComposeBarLineHeight) / 2,
				st::historyComposeBarLine,
				st::historyComposeBarLineHeight)),
			st::historyComposeBarLine / 2.,
			st::historyComposeBarLine / 2.);
	}
	left += st::historyComposeBarLine + st::historyComposeBarLineSkip;

	if (_barThumb) {
		const auto size = st::historyComposeBarThumb;
		const auto box = myrtlrect(left, rect.y() + (rect.height() - size) / 2, size, size);
		const auto ratio = devicePixelRatioF();
		auto image = _barThumb->request(box.size() * ratio);
		if (image.isNull()) {
			image = _barThumb->preview();
		}
		if (!image.isNull()) {
			if (_barThumbPrepared.isNull() || _barThumbKey != image.cacheKey()) {
				_barThumbPrepared = Ui::Images::Round(
					Ui::Images::PrepareCover(image, box.size(), ratio),
					st::historyComposeBarThumbRadius);
				_barThumbKey = image.cacheKey();
			}
			p.drawImage(box.topLeft(), _barThumbPrepared);
			left += size + st::historyComposeBarLineSkip;
		}
	}

	const auto rightEdge = rect.x() + rect.width() - padding.right() - _barClose->width() - 4;
	const auto available = rightEdge - left;
	if (available <= 0) {
		return;
	}
	const auto name = [&](const Data::Message &message) {
		return _resolveName ? _resolveName(message) : QString();
	};
	auto title = QString();
	auto text = QString();
	if (_mode == Mode::Edit) {
		title = tr::lng_chat_edit_message(tr::now);
		text = Support::MessagePreview(_target);
	} else if (_mode == Mode::Reply) {
		title = tr::lng_chat_reply(tr::now, lt_name, name(_target));
		text = _quote ? _quote->text.simplified() : Support::MessagePreview(_target);
	} else {
		const auto count = int(_forwards.size());
		title = tr::lng_history_forward_count(tr::now, lt_count, count);
		if (count == 1) {
			text = Support::MessagePreview(_target);
		} else {
			auto names = QStringList();
			for (const auto &message : std::as_const(_forwards)) {
				const auto one = name(message);
				if (!one.isEmpty() && !names.contains(one)) {
					names.push_back(one);
				}
			}
			text = names.join(u", "_s);
		}
	}
	p.setPen(st::historyComposeBarTitleFg);
	p.setFont(st::historyComposeBarTitleFont);
	p.drawTextLeft(
		left,
		rect.y() + st::historyComposeBarTitleTop,
		width(),
		st::historyComposeBarTitleFont->elided(title, available));
	p.setPen(st::historyComposeBarTextFg);
	p.setFont(st::historyComposeBarTextFont);
	p.drawTextLeft(
		left,
		rect.y() + st::historyComposeBarTextTop,
		width(),
		st::historyComposeBarTextFont->elided(text, available));
}

void ComposeControls::paintPreview(Painter &p, QRect rect) {
	const auto &padding = st::historyComposeBarPadding;
	const auto &icon = st::historyComposeLinkIcon;
	auto left = rect.x() + padding.left();
	icon.paint(p, left, rect.y() + (rect.height() - icon.height()) / 2, width());
	left += icon.width() + st::historyComposeBarIconSkip;
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::historyReplyBarFg);
		p.drawRoundedRect(
			QRectF(myrtlrect(
				left,
				rect.y() + (rect.height() - st::historyComposeBarLineHeight) / 2,
				st::historyComposeBarLine,
				st::historyComposeBarLineHeight)),
			st::historyComposeBarLine / 2.,
			st::historyComposeBarLine / 2.);
	}
	left += st::historyComposeBarLine + st::historyComposeBarLineSkip;
	const auto rightEdge = rect.x()
		+ rect.width()
		- padding.right()
		- _previewClose->width()
		- 4;
	const auto available = rightEdge - left;
	if (available <= 0) {
		return;
	}
	const auto &data = _preview.data;
	const auto title = !data.siteName.isEmpty()
		? data.siteName
		: !data.title.isEmpty()
		? data.title
		: tr::lng_history_link_preview(tr::now);
	const auto text = (!data.title.isEmpty() && !data.siteName.isEmpty())
		? data.title
		: !data.description.isEmpty()
		? data.description.simplified()
		: data.url;
	p.setPen(st::historyComposeBarTitleFg);
	p.setFont(st::historyComposeBarTitleFont);
	p.drawTextLeft(
		left,
		rect.y() + st::historyComposeBarTitleTop,
		width(),
		st::historyComposeBarTitleFont->elided(title, available));
	p.setPen(st::historyComposeBarTextFg);
	p.setFont(st::historyComposeBarTextFont);
	p.drawTextLeft(
		left,
		rect.y() + st::historyComposeBarTextTop,
		width(),
		st::historyComposeBarTextFont->elided(text, available));
}

void ComposeControls::paintRecording(Painter &p, QRect rect) {
	const auto &padding = st::historyComposePadding;
	auto left = rect.x() + padding.left() + _recordCancel->width() + st::historyRecordSkip;
	const auto middle = rect.y() + rect.height() - _send->height() / 2;
	// The blinking dot.
	{
		const auto phase = (Ui::Now() % 1200) / 1200.;
		const auto opacity = Ui::AnimationsDisabled()
			? 1.
			: (0.35 + 0.65 * std::abs(1. - 2. * phase));
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::historyRecordDotFg);
		p.setOpacity(opacity);
		const auto size = st::historyRecordDot;
		p.drawEllipse(myrtlrect(left, middle - size / 2, size, size));
		p.setOpacity(1.);
		left += size + st::historyRecordSkip;
	}
	const auto time = DurationText(_recordedMs);
	const auto &timeFont = st::historyRecordTimeFont;
	p.setPen(st::textPrimary);
	p.setFont(timeFont);
	p.drawTextLeft(left, middle - timeFont->height / 2, width(), time);
	left += timeFont->width(time) + st::historyRecordSkip * 2;

	const auto rightEdge = rect.x() + rect.width() - padding.right() - _send->width();
	const auto available = rightEdge - left;
	if (available > 40) {
		const auto &font = st::historyRecordHintFont;
		p.setPen(st::historyRecordHintFg);
		p.setFont(font);
		p.drawTextLeft(
			left,
			middle - font->height / 2,
			width(),
			font->elided(tr::lng_history_record_hint(tr::now), available));
	}
}

#ifdef ELEZA_DEBUG_TOOLS
// ---- debug hooks

void ComposeControls::debugShowPanel(bool stickers) {
	if (stickers) {
		_panel->showStickers();
	}
	_panel->showAbove(_emoji);
}

void ComposeControls::debugShowAttachMenu() {
	showAttachMenu();
}
#endif // ELEZA_DEBUG_TOOLS

} // namespace History
