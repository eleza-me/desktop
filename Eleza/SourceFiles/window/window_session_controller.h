// Window::SessionController — the navigation API of the session layout
// (nav rail | side column | main column). Every section gets a pointer to it.
//
//   controller->showTab(Window::Tab::Chats);
//
//   // Open something in the main column of the current tab; it is pushed on that tab's
//   // back stack with a slide animation:
//   controller->showMainSection([=](QWidget *parent, Window::SessionController *controller) {
//       return Ui::Owned<Window::SectionWidget>(new History::Widget(parent, controller, chatId));
//   });
//   controller->showMainSection(factory, { .stack = false });         // replace the stack
//   controller->showMainSection(factory, { .tab = Window::Tab::Eli }); // in another tab
//
//   controller->showBack();          // pop (Esc and the top bar's back button do this)
//   controller->clearMainStack();    // back to the tab's empty state
//
//   controller->show(Ui::Box<MyBox>(args...));                 // modal box
//   controller->show(Ui::MakeConfirmBox({ ... }));
//   controller->showSheet(Ui::Box<MySheet>(args...));          // side sheet
//   controller->showToast(tr::lng_copied(tr::now));
//   controller->setTabBadge(Window::Tab::Chats, unreadCount);  // rail badge
//
//   controller->setSideSection(tab, factory);  // replace a tab's side column at runtime
//                                              // (normally: Window::RegisterTab at startup)
//   controller->openRoute(u"chat/42"_s);       // through the registered route handlers
//
// Every tab keeps its own side section and its own main stack while the session lives,
// so switching tabs returns to where the user was. In the single column mode (window
// narrower than st::layoutSingleColumnWidth) the main section covers the whole window
// and showBack() returns to the list.
//
// session() is the signed-in session (it outlives the controller and every section):
//
//   auto &data = controller->session().data();     // Data::Session: the stores
//   controller->session().api().request(...)       // plain HTTP
//   controller->requestLogout();                   // «خروج» with its confirmation
#pragma once

#include "ui/effects/animation_value.h"
#include "ui/layers/layer_widget.h"
#include "ui/toast/toast.h"
#include "window/window_registry.h"

#include <QtCore/QObject>
#include <QtCore/QStringList>

namespace Main {
class Account;
class Session;
} // namespace Main

namespace Ui {
class BoxContent;
} // namespace Ui

namespace Window {

class MainLayout;
class MainWindow;

// Where the user is: the tab and the routes of every tab's main stack (bottom
// to top). Taken before the window content is rebuilt and restored after.
struct LayoutState {
	Tab tab = Tab::Chats;
	QStringList stacks[kTabsCount];
};

struct SectionShow {
	// true: push over the current section; false: replace the whole stack.
	bool stack = true;
	anim::type animated = anim::type::normal;
	// The tab whose main column gets the section (and which becomes current).
	std::optional<Tab> tab = std::nullopt;
};

class SessionController final : public QObject {
	Q_OBJECT

public:
	SessionController(MainWindow *window, Main::Session *session);
	~SessionController();

	[[nodiscard]] Main::Session &session() const {
		return *_session;
	}
	[[nodiscard]] Main::Account &account() const;
	[[nodiscard]] MainWindow *window() const {
		return _window;
	}
	// The widget that hosts the layout; parent for dropdowns anchored in-window.
	[[nodiscard]] Ui::RpWidget *bodyWidget() const;

	// ---- tabs
	// Like a click on the rail: on the current tab it returns to the tab's
	// list (closes what is open in the main column).
	void showTab(Tab tab);
	// Only makes the tab current; what it has open stays.
	void activateTab(Tab tab);
	[[nodiscard]] Tab currentTab() const;
	void setSideSection(Tab tab, SectionFactory factory);
	[[nodiscard]] SectionWidget *sideSection(Tab tab) const; // null until first shown
	void setTabBadge(Tab tab, int count, bool muted = false);
	// Side sub-tabs (window_embeds.h): asks the side column of `tab` to show
	// its sub-tab `id` ("topics"). The host of the segmented control listens
	// to sideSubTabRequested() and, when it is created later, takes the
	// pending request once with takeRequestedSideSubTab().
	void requestSideSubTab(Tab tab, const QString &id);
	[[nodiscard]] QString takeRequestedSideSubTab(Tab tab);

	// ---- the main column
	void showMainSection(SectionFactory factory, SectionShow options = {});
	// Returns false if there was nothing to go back from.
	bool showBack();
	void clearMainStack();
	void clearMainStack(Tab tab);
	[[nodiscard]] SectionWidget *mainSection() const; // the visible one, or null
	[[nodiscard]] int mainStackDepth() const;
	[[nodiscard]] bool isSingleColumn() const;
	// A section kept further back (a chat under its profile) comes to the
	// top of its stack instead of being opened once more; false: none.
	bool showExistingSection(const QString &sectionKey);
	// The strip on top of the main column (the «now playing» bar), owned
	// by the layout from now on; null removes it.
	void setMainStrip(QWidget *strip);

	[[nodiscard]] LayoutState saveState() const;
	void restoreState(const LayoutState &state);

	// ---- layers and toasts
	void show(
		Ui::object_ptr<Ui::BoxContent> box,
		Ui::LayerOption option = Ui::LayerOption::KeepOther);
	void showSheet(
		Ui::object_ptr<Ui::BoxContent> content,
		Ui::LayerOption option = Ui::LayerOption::KeepOther);
	void hideLayer();
	void hideLayers();
	void showToast(const QString &text);
	void showToast(Ui::Toast::Config config);

	// ---- routes (window_routes.h)
	// "chats", "chat/<id>", "u/<handle>", "eli/post/<id>"… Returns false for a
	// string that is not a route.
	bool openRoute(const QString &route);

	// ---- account
	// Asks for a confirmation, then signs out (the app returns to the intro).
	void requestLogout();

	// Called by the layout.
	void setLayout(MainLayout *layout);
	void notifyTabChanged(Tab tab);
	void notifyMainSectionChanged();
	void notifyColumnsChanged(bool singleColumn);

Q_SIGNALS:
	void tabChanged(Window::Tab tab);
	void sideSubTabRequested(Window::Tab tab, const QString &id);
	void mainSectionChanged();
	// The layout switched between one and several columns.
	void columnsChanged(bool singleColumn);

private:
	void refreshBadges();

	MainWindow *_window = nullptr;
	Main::Session *_session = nullptr;
	MainLayout *_layout = nullptr;
	QString _requestedSideSubTabs[kTabsCount];

};

} // namespace Window
