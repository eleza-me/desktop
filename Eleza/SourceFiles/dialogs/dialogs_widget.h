// Dialogs::Widget — the side column of the «گفتگو» tab: the title (which turns
// into «در حال اتصال…» while the app is not online), the search field, the
// segmented control of the sub-tabs («چت‌ها» + whatever other modules registered
// through Window::RegisterSideSubTab) and the chat list with its folders
// («درخواست‌ها», «بایگانی‌شده»).
#pragma once

#include "dialogs/dialogs_logic.h"
#include "window/window_embeds.h"
#include "window/window_section_widget.h"

#include <QtCore/QPointer>

#include <vector>

namespace Support {
class ConnectionState;
} // namespace Support

namespace Ui {
class EmptyPlaceholder;
class IconButton;
class ScrollArea;
class SearchField;
class SkeletonList;
class TabBar;
class TopBar;
} // namespace Ui

namespace Dialogs {

class InnerWidget;

class Widget final : public Window::SectionWidget {
public:
	Widget(QWidget *parent, Window::SessionController *controller);
	~Widget();

	// «درخواست‌ها» / «بایگانی‌شده» replace the list inside the column.
	void showFolder(Folder folder);
	[[nodiscard]] Folder folder() const;
	void focusSearch();
	// Ctrl+Tab and friends: the chat after / before the open one.
	void jumpToChat(int direction);

	QString sectionKey() const override;
	void setInnerFocus() override;
	bool onBackRequest() override;
	bool handleCommand(Window::Command command) override;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	class Banner;
	struct SubTab {
		Window::SideSubTab descriptor;
		QPointer<Ui::RpWidget> widget; // null for «چت‌ها» and until first shown
	};

	void setupSubTabs();
	void refreshTabTitles();
	void showSubTab(int index, bool remember);
	[[nodiscard]] bool chatsTabShown() const;
	void updateControlsGeometry();
	void refreshTopBar();
	void refreshTitle();
	void refreshState();
	void refreshActiveChat();
	void queryChanged(const QString &text);
	void openChat(const QString &id);
	bool handleKey(QKeyEvent *e);
	void searchCancelled();

	Ui::TopBar *_bar = nullptr;
	QPointer<Support::ConnectionState> _connection;
	Ui::SearchField *_search = nullptr;
	Ui::TabBar *_tabs = nullptr;
	Banner *_banner = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	InnerWidget *_inner = nullptr;
	QPointer<Ui::SkeletonList> _skeleton;
	QPointer<Ui::EmptyPlaceholder> _empty;
	int _emptyState = 0;
	std::vector<SubTab> _subTabs;
	int _subTab = 0;
	QRect _paneRect;

};

} // namespace Dialogs
