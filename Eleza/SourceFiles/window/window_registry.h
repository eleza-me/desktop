// The registry feature modules use to plug into the window.
//
//   // dialogs/dialogs_module.cpp
//   void Dialogs::Register() {
//       Window::RegisterTab(Window::Tab::Chats, [](QWidget *parent, Window::SessionController *controller) {
//           return Ui::Owned<Window::SectionWidget>(new Dialogs::Widget(parent, controller));
//       });
//       Window::RegisterRouteHandler([](Window::SessionController *controller, const QString &route) {
//           if (!route.startsWith(u"chat/"_s)) return false;
//           controller->showMainSection(History::Factory(route.mid(5)), { .tab = Window::Tab::Chats });
//           return true;
//       });
//   }
//   // intro/intro_module.cpp
//   void Intro::Register() {
//       Window::RegisterIntro([](QWidget *parent, Window::MainWindow *window) {
//           return Ui::Owned<Ui::RpWidget>(new Intro::Widget(parent, window));
//       });
//   }
//
// and one line in core/core_modules.cpp calls the module's Register(). A tab without
// a registered factory shows a placeholder.
#pragma once

#include "ui/rp_widget.h"

#include <QtCore/QString>

#include <functional>
#include <optional>

namespace Window {

class MainWindow;
class SectionWidget;
class SessionController;

// The tabs of the navigation rail, top to bottom (Settings sits at the bottom).
enum class Tab {
	Eli, // the feed
	Chats, // «گفتگو»: chats and open conversations
	Search,
	Me, // own profile
	Settings,
};
inline constexpr auto kTabsCount = 5;

// "eli", "chats", "search", "me", "settings" — used in routes and settings.
[[nodiscard]] QString TabId(Tab tab);
[[nodiscard]] std::optional<Tab> TabFromId(const QString &id);

using SectionFactory = std::function<Ui::object_ptr<SectionWidget>(
	QWidget *parent,
	SessionController *controller)>;
using IntroFactory = std::function<Ui::object_ptr<Ui::RpWidget>(
	QWidget *parent,
	MainWindow *window)>;
// Returns true if it opened the route ("chat/<id>", "post/<id>", "profile/<handle>"…).
using RouteHandler = std::function<bool(
	SessionController *controller,
	const QString &route)>;

// The side column content of a tab. Registered once at startup.
void RegisterTab(Tab tab, SectionFactory factory);
// The signed-out flow (phone -> code -> password -> profile).
void RegisterIntro(IntroFactory factory);
void RegisterRouteHandler(RouteHandler handler);

// ---- used by the window code
[[nodiscard]] SectionFactory RegisteredTabFactory(Tab tab); // null if none
[[nodiscard]] bool HasIntro();
[[nodiscard]] Ui::object_ptr<Ui::RpWidget> CreateIntro(
	QWidget *parent,
	MainWindow *window);
[[nodiscard]] bool HandleRoute(
	SessionController *controller,
	const QString &route);

} // namespace Window
