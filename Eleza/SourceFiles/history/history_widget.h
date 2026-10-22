// The chat screen: top bar (or the search bar), the pinned message, the
// conversation banner, the message list and the composer (or the notice
// that replaces it when one cannot write).
//
// A chat with a bot that has no message yet shows the bot's description in
// the middle and «شروع» instead of the composer until the first message;
// opened from a deep link (eleza.me/<bot>?start=<payload>) it shows «شروع»
// even when it has messages, and the button sends "/start <payload>".
//
//   controller->showMainSection(History::Factory(chatId, seq), { .tab = Window::Tab::Chats });
#pragma once

#include "window/window_registry.h"
#include "ui/effects/animations.h"
#include "window/window_section_widget.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include <optional>

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class PopupMenu;
class ScrollArea;
} // namespace Ui

namespace History {

class BotIntroCard;
class BottomNotice;
class ComposeControls;
class ConversationBanner;
class ListWidget;
class PinnedBar;
class SearchBar;
class TopBar;

// `botStart`: opened from a bot's deep link (its payload, may be empty).
[[nodiscard]] Window::SectionFactory Factory(
	const QString &chatId,
	qint64 seq = 0,
	std::optional<QString> botStart = std::nullopt);

class Widget final : public Window::SectionWidget {
	Q_OBJECT

public:
	Widget(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &chatId,
		qint64 aroundSeq = 0,
		std::optional<QString> botStart = std::nullopt);
	~Widget();

	[[nodiscard]] QString sectionKey() const override;
	void showFinished() override;
	void setInnerFocus() override;
	[[nodiscard]] bool onBackRequest() override;
	void setBackButtonShown(bool shown) override;

	[[nodiscard]] const QString &chatId() const {
		return _chatId;
	}
	// The chat is already open and a route asks for a message in it.
	void showMessage(qint64 seq);
	// The chat is already open and a deep link asks to start its bot.
	void startBot(const QString &payload);

	// ---- debug hooks (screenshots, the stress test)
	[[nodiscard]] ListWidget *list() const {
		return _list;
	}
	[[nodiscard]] ComposeControls *compose() const {
		return _compose;
	}
	void showSearch();
	void showMoreMenu();
	void showThemeBox();
#ifdef ELEZA_DEBUG_TOOLS
	void debugShowDropArea();
#endif // ELEZA_DEBUG_TOOLS
	int toastBottomSkip() const override;
	bool handleCommand(Window::Command command) override;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	void dragEnterEvent(QDragEnterEvent *e) override;
	void dragMoveEvent(QDragMoveEvent *e) override;
	void dragLeaveEvent(QDragLeaveEvent *e) override;
	void dropEvent(QDropEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	class DownButton;
	class DropArea;
	class StateView;

	enum class Bottom {
		None,
		Compose,
		Request,
		Blocked,
		Deleted,
		Ended,
		Readonly,
		GroupRestricted, // a group: I may not send here (maybe until a time)
		GroupWait,       // a group: slow mode, I still wait
		BotStart,        // a bot that was not started here: «شروع»
	};

	void setupList();
	void setupTopBar();
	void setupBars();
	void setupCompose();
	void updateLayout();
	void refreshChat();
	void refreshBottom();
	void refreshState();
	void refreshDownButton();
	void refreshMentionsButton();
	void placeFloatingButtons();
	void pressDown();
	void pressMentions();
	void refreshActive();
	void refreshRelationship();
	void selectionChanged();
	void hideSearch();
	void fillMuteMenu(Ui::PopupMenu *menu);
	void confirmBlock();
	void unpin(qint64 seq);
	void copySelection();
	void forwardSelection();
	void deleteSelection();
	void pressBotStart();
	void refreshBotInfo();
	void showSwitchInline(const QString &text);
	void inlineQueryRequested(const QString &text, bool pickChat);
	[[nodiscard]] bool canAttach() const;

	Main::Session *_session = nullptr;
	QString _chatId;
	qint64 _aroundSeq = 0;

	TopBar *_topBar = nullptr;
	SearchBar *_search = nullptr;
	PinnedBar *_pinned = nullptr;
	ConversationBanner *_banner = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	ListWidget *_list = nullptr;
	ComposeControls *_compose = nullptr;
	BottomNotice *_notice = nullptr;
	DownButton *_down = nullptr;
	DownButton *_mentions = nullptr; // a group: «@»
	Ui::Animations::Simple _mentionsSlide;
	bool _downStopped = false; // «to the end» made its first stop
	QVector<qint64> _mentionQueue;
	qint64 _mentionShown = 0;
	bool _mentionsLoading = false;
	DropArea *_drop = nullptr;
	StateView *_state = nullptr;
	BotIntroCard *_botIntro = nullptr;
	QPointer<Ui::PopupMenu> _menu;

	Bottom _bottom = Bottom::None;
	bool _shown = false;         // the slide animation finished
	bool _blockedByYou = false;
	bool _relationshipKnown = false;
	QString _relationshipFor;
	bool _headerRequested = false;
	bool _requestBusy = false;
	bool _leaving = false;
	bool _group = false;
	QTimer _groupTimer; // the countdown of slow mode / a restriction
	QString _groupNotice;

	// ---- a chat with a bot
	bool _botStartPending = false; // opened from a deep link
	QString _botStartPayload;
	bool _botStartSent = false;
	QString _botId;

};

} // namespace History
