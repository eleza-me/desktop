// The composer under the message list: the field with its buttons (attach,
// emoji / stickers, send or microphone), the bar above it (what is being
// replied to / edited / forwarded, the link preview of the text) and the
// voice recording strip. In a chat with a bot the bot's reply keyboard (the
// markup of its newest message that carries one) sits under the field.
//
//   _compose = Ui::CreateChild<ComposeControls>(this, controller, chatId, this);
//   connect(_compose, &Ui::RpWidget::heightChanged, ...);   // grows with the text
//   _compose->replyTo(message, quote);  _compose->editMessage(message);
//
// Spoilers: the selection is marked with Ctrl+Shift+P or the field's context
// menu; the ranges live next to the text (UTF-16 offsets), follow every edit
// (History::AdjustEntities) and are sent as entities.
//
// Mentions (a group, on a server with the second release of groups): «@»
// lists the members whose name or handle matches what is typed after it
// (history_mentions.h); a picked member's name goes into the text and the
// range is sent as a mention entity with the member's id. Editing inside
// the name turns it back into plain text. The rules are Data::MentionRules.
//
// Bots (history_compose_bots.cpp, the rules in history_bot_logic.h):
//   * in a chat with a bot, «/» lists its commands above the field (filtered
//     as the name is typed; the «/» button shows them all); Enter sends the
//     picked command, Tab puts it into the field;
//   * inline mode in any chat of two: «@<bot> <query>» asks the bot (after a
//     pause in typing) and lists its results above the field, page by page;
//     a picked result goes out through the outbox as `inline_result`;
//   * the reply keyboard follows `resize_keyboard`, a button next to the
//     field hides / shows it (a new keyboard shows itself, a persistent one
//     comes back after every message), `remove_keyboard` takes it away.
//
// «Send without sound»: the context menu of the send button; the message
// goes out with `silent` and alerts nobody.
//
// Drafts: the text is saved two seconds after the last keystroke and when the
// chat is left; while the field has the focus a draft that arrives from
// another device does not replace what is being typed. Anonymous
// conversations keep their drafts on this device only.
#pragma once

#include "base/timer.h"
#include "data/data_bots.h"
#include "data/data_types.h"
#include "history/history_bot_logic.h"
#include "history/history_compose_logic.h"
#include "ui/effects/animations.h"
#include "ui/image/image_source.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"

#include <QtCore/QHash>
#include <QtCore/QPointer>

#include <memory>

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class IconButton;
class InputField;
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace History {

class BotKeyboard;
class BotSuggestPanel;
class MentionsDropdown;
class TabbedPanel;
class VoiceRecorder;

// Messages chosen with «Forward» wait here until the target chat's composer
// takes them (it shows them in its bar and sends them with the next message).
void SetPendingForward(
	Main::Session *session,
	const QString &toChat,
	const QVector<Data::Message> &messages);

// A text for the composer of another chat (a «switch inline» button): the
// composer takes it when it is created or focused, like forwards.
void SetPendingPrefill(
	Main::Session *session,
	const QString &toChat,
	const QString &text);

class ComposeControls final : public Ui::RpWidget {
	Q_OBJECT

public:
	// `panelParent` hosts the emoji / sticker panel (it floats over the list).
	ComposeControls(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &chatId,
		QWidget *panelParent);
	~ComposeControls();

	void setInnerFocus();
	[[nodiscard]] bool fieldFocused() const;
	// A key pressed while the focus was elsewhere in the chat: it goes
	// into the field, no character is lost.
	void typeFromOutside(QKeyEvent *e);
	// Reply / quote from the list.
	void replyTo(const Data::Message &message, const std::optional<Data::Quote> &quote);
	void editMessage(const Data::Message &message);
	// Files and pictures from the clipboard / a drop / the attach menu.
	void attachFiles(const QStringList &paths);
	void attachImage(const QImage &image);
	// Esc: stops recording, leaves the edit / reply / forward state, hides the
	// panel. False when there was nothing to cancel.
	bool cancelCurrent();
	// Messages left by SetPendingForward() for this chat go into the bar.
	void takePendingForward();
	// Saves the draft right now (the chat is being left).
	void flushDraft();
	// Text left by SetPendingPrefill() for this chat goes into the field.
	void takePendingPrefill();
	// Puts `text` into the field (replacing what was typed), the caret at
	// its end: «@bot query» of a «switch inline» button.
	void prefill(const QString &text);
	// A «/command» clicked in a bot's message: sent like a typed text.
	void sendCommand(const QString &command);
	// Inline mode may be used here: a chat of two, not anonymous, bots on.
	[[nodiscard]] bool inlineModeAllowed() const;
	// The sender's name for the reply bar comes from the list.
	void setNameResolver(Fn<QString(const Data::Message &)> resolver) {
		_resolveName = std::move(resolver);
	}
	void setLastEditable(Fn<std::optional<Data::Message>()> provider) {
		_lastEditable = std::move(provider);
	}
	[[nodiscard]] TabbedPanel *panel() const {
		return _panel;
	}
	void updatePanelPosition();
	// Debug hooks of the screenshots.
#ifdef ELEZA_DEBUG_TOOLS
	void debugShowPanel(bool stickers);
	void debugShowAttachMenu();
#endif // ELEZA_DEBUG_TOOLS

Q_SIGNALS:
	// A message went into the outbox: the list shows its end.
	void sent();
	void jumpRequested(qint64 seq);
	void scrollRequested(bool up);
	// Ctrl+C with nothing selected in the field.
	void copyRequested();
	void locationRequested();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;
	int resizeGetHeight(int newWidth) override;

private:
	enum class Mode {
		None,
		Reply,
		Edit,
		Forward,
	};
	struct Preview {
		QString url;      // the address the card is for
		Data::LinkPreview data;
		bool loading = false;
		bool ready = false;
	};

	void setupField();
	void setupButtons();
	void setupDraft();
	void updateControls();
	void updateFieldGeometry();
	void layoutControls(int width, int height);
	[[nodiscard]] int barHeight() const;
	[[nodiscard]] int previewHeight() const;
	[[nodiscard]] QRect contentRect() const; // the centred column
	void paintBar(Painter &p, QRect rect);
	void paintPreview(Painter &p, QRect rect);
	void paintRecording(Painter &p, QRect rect);

	// ---- text and spoilers
	void fieldChanged();
	void applySpoilerMarks();
	void toggleSpoiler();
	void setFieldText(const QString &text, const QVector<Data::Entity> &entities);
	[[nodiscard]] Data::Draft currentDraft() const;
	void fillFieldMenu(Ui::PopupMenu *menu);
	bool fieldKeyPress(QKeyEvent *e);
	bool pasteFromClipboard();

	// ---- modes
	void setMode(Mode mode);
	void cancelMode();
	void refreshBarThumb();

	// ---- sending
	void submit();
	void send(bool silent = false);
	void showSendMenu(QPoint globalPosition);
	void saveEdit();
	void sendForwards();
	void sendSticker(const Data::Sticker &sticker);
	void showAttachMenu();
	void pickFiles(bool media);
	void typing();
	void showToast(const QString &text);

	// ---- mentions (a group)
	void updateMentions();
	void hideMentions();
	void showMentions(int request, const QVector<Data::User> &users);
	void pickMention(const Data::User &user);
	void checkMentions();
	void placeMentions();
	[[nodiscard]] QVector<Data::Entity> entitiesToSend() const;

	// ---- the bot's reply keyboard
	void refreshBotKeyboard();
	[[nodiscard]] int keyboardHeight() const;
	[[nodiscard]] bool keyboardShown() const;
	void toggleBotKeyboard();
	void keyboardAfterSend();

	// ---- bots: commands and inline mode (history_compose_bots.cpp)
	void setupBots();
	void refreshBotChat();
	void applyBotInfo(const Data::BotInfo &info);
	void updateBotSuggestions();
	void hideBotPanel();
	void placeBotPanel();
	void toggleCommands();
	void pickCommand(const Data::BotCommand &command, bool insert);
	void inlineText(const QString &handle, const QString &query);
	void leaveInline();
	void runInlineQuery(const QString &offset);
	void inlineNeedMore();
	void pickInlineResult(const Data::InlineResult &result);
	[[nodiscard]] QRect suggestionsAnchor() const;
	[[nodiscard]] int trailingButtonsWidth() const;

	// ---- link preview
	void checkPreview();
	void requestPreview(const QString &url);
	void dismissPreview();

	// ---- voice
	void startRecording();
	void stopRecording(bool send);
	[[nodiscard]] bool recording() const;

	// ---- drafts
	void applyRemoteDraft();
	[[nodiscard]] QString localDraftKey() const;

	Window::SessionController *_controller = nullptr;
	Main::Session *_session = nullptr;
	QString _chatId;
	bool _conversation = false;

	Ui::InputField *_field = nullptr;
	Ui::IconButton *_attach = nullptr;
	Ui::IconButton *_emoji = nullptr;
	Ui::IconButton *_send = nullptr;
	Ui::IconButton *_mic = nullptr;
	Ui::IconButton *_barClose = nullptr;
	Ui::IconButton *_previewClose = nullptr;
	Ui::IconButton *_recordCancel = nullptr;
	TabbedPanel *_panel = nullptr;
	BotKeyboard *_keyboard = nullptr;
	Ui::IconButton *_keyboardToggle = nullptr;
	Ui::IconButton *_botCommands = nullptr;
	qint64 _keyboardUsedSeq = 0;          // a one-time keyboard that was pressed
	bool _keyboardOneTime = false;
	bool _keyboardPersistent = false;
	ReplyKeyboardView _keyboardView;      // which keyboard, put away or not

	// ---- bots
	struct InlineState {
		bool active = false;
		QString handle;                   // lower-case
		QString botId;
		QString placeholder;
		QString query;
		QVector<Data::InlineResult> results;
		QHash<QString, QString> queryIds; // result id -> the query it came from
		QString next;                     // the offset of the next page
		bool loadingMore = false;
		int generation = 0;               // an answer for an older query is dropped
	};
	BotSuggestPanel *_botPanel = nullptr;
	bool _botChat = false;                // the peer is a bot
	QString _botId;
	QVector<Data::BotCommand> _commands;
	InlineState _inlineMode;
	base::Timer _inlineTimer;
	QPointer<Ui::PopupMenu> _attachMenu;

	// ---- the text
	QString _text;
	QVector<Data::Entity> _spoilers;
	// Picked members: type "mention", `user` = the person, `url` = the name
	// as it was written (a range that no longer reads so is dropped).
	QVector<Data::Entity> _mentions;
	MentionsDropdown *_mentionsBox = nullptr;
	QWidget *_panelParent = nullptr;
	QString _mentionShownFor; // "<at>:<text>" of the query the list answers
	int _mentionRequest = 0;
	bool _group = false;
	QPointer<Ui::PopupMenu> _sendMenu;
	int _changeHint = -1;
	bool _settingText = false;

	// ---- the mode
	Mode _mode = Mode::None;
	Data::Message _target;                // replied to / edited
	std::optional<Data::Quote> _quote;
	QVector<Data::Message> _forwards;
	Data::Draft _stashed;                 // the draft that waits while editing
	bool _editSaving = false;
	std::shared_ptr<Ui::ImageSource> _barThumb;
	QMetaObject::Connection _barThumbConnection;
	QImage _barThumbPrepared;
	qint64 _barThumbKey = 0;
	Fn<QString(const Data::Message &)> _resolveName;
	Fn<std::optional<Data::Message>()> _lastEditable;

	// ---- link preview
	Preview _preview;
	QString _previewDismissed;            // the address the user closed the card of
	base::Timer _previewTimer;
	int _previewRequest = 0;

	// ---- drafts
	std::unique_ptr<DraftSaver> _draft;
	base::Timer _draftTimer;
	bool _draftReady = false;

	// ---- voice
	std::unique_ptr<VoiceRecorder> _recorder;
	qint64 _recordedMs = 0;
	Ui::Animations::Basic _recordPulse;

	qint64 _typingSentAt = 0;

};

} // namespace History
