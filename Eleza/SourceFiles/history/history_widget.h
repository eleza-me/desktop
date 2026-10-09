// The chat screen: top bar (or the search bar), the pinned message, the
// conversation banner, the message list and the composer (or the notice
// that replaces it when one cannot write).
//
//   controller->showMainSection(History::Factory(chatId, seq), { .tab = Window::Tab::Chats });
#pragma once

#include "window/window_registry.h"
#include "window/window_section_widget.h"

#include <QtCore/QPointer>

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class PopupMenu;
class ScrollArea;
} // namespace Ui

namespace History {

class BottomNotice;
class ComposeControls;
class ConversationBanner;
class ListWidget;
class PinnedBar;
class SearchBar;
class TopBar;

[[nodiscard]] Window::SectionFactory Factory(const QString &chatId, qint64 seq = 0);

class Widget final : public Window::SectionWidget {
	Q_OBJECT

public:
	Widget(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &chatId,
		qint64 aroundSeq = 0);
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
	void debugShowDropArea();

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
	void refreshActive();
	void refreshRelationship();
	void selectionChanged();
	void hideSearch();
	void fillMuteMenu(Ui::PopupMenu *menu);
	void confirmBlock();
	void confirmEndConversation();
	void unpin(qint64 seq);
	void copySelection();
	void forwardSelection();
	void deleteSelection();
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
	DropArea *_drop = nullptr;
	StateView *_state = nullptr;
	QPointer<Ui::PopupMenu> _menu;

	Bottom _bottom = Bottom::None;
	bool _shown = false;         // the slide animation finished
	bool _blockedByYou = false;
	bool _relationshipKnown = false;
	QString _relationshipFor;
	bool _headerRequested = false;
	bool _requestBusy = false;
	bool _leaving = false;

};

} // namespace History
