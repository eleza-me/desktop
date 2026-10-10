// The side column of the «الی» tab: the feed. «برای تو» / «دنبال‌شده» keep
// their own scroll positions, a compose entry sits on top of each, the bell
// shows unread notifications, and a pill offers the new posts of the people I
// follow with their faces (it never moves the list under the reader).
//
// Keyboard: J / K / L / R / B / Enter on the list, N composes, F5 (Ctrl+R)
// refreshes, «.» shows the new posts. Files dropped on the column open
// compose with them attached.
#pragma once

#include "base/timer.h"
#include "window/window_section_widget.h"

namespace Ui {
class IconButton;
class ScrollArea;
class TabBar;
class TopBar;
class UnreadBadge;
class VerticalLayout;
} // namespace Ui

namespace Support {
class ConnectionTitle;
} // namespace Support

namespace Eli {

class PostList;

class HomeSection final : public Window::SectionWidget {
public:
	HomeSection(QWidget *parent, Window::SessionController *controller);
	~HomeSection();

	void setInnerFocus() override;
	void showFinished() override;
	bool handleCommand(Window::Command command) override;

	// «N پست تازه» as if the server told (debug route, tests).
	void showNewPosts(int count);

protected:
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void dragEnterEvent(QDragEnterEvent *e) override;
	void dropEvent(QDropEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	class Entry;
	class NewPill;
	class LoadingLine;
	struct Pane {
		Ui::ScrollArea *scroll = nullptr;
		Ui::VerticalLayout *content = nullptr;
		Entry *entry = nullptr;
		PostList *list = nullptr;
		int lastScrollTop = 0;
	};

	void createPane(int index);
	void setTab(int index, bool save);
	void refreshCurrent();
	void updateBadge();
	void updateActivePost();
	void updateLoading();
	void placeBadge();
	void newPostsArrived(int count);
	void applyNewPosts();
	void clearNewPosts();
	void watchNew();
	[[nodiscard]] qint64 newSince() const;
	[[nodiscard]] Pane &current() {
		return _panes[_tab];
	}

	Ui::TopBar *_bar = nullptr;
	Support::ConnectionTitle *_title = nullptr;
	Ui::IconButton *_bell = nullptr;
	Ui::UnreadBadge *_badge = nullptr;
	Ui::TabBar *_tabs = nullptr;
	LoadingLine *_loadingLine = nullptr;
	NewPill *_pill = nullptr;
	Pane _panes[2];
	int _tab = 0;
	int _tabOnPress = -1;

	qint64 _openedAt = 0;
	qint64 _newestFollowing = 0;
	int _newCount = 0;
	bool _pillHiddenByScroll = false;
	qint64 _lastCheck = 0;
	bool _authorsLoading = false;

};

} // namespace Eli
