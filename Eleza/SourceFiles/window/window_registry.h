// The registry feature modules use to plug into the window.
//
//   // dialogs/dialogs_module.cpp
//   void Dialogs::Register() {
//       Window::RegisterTab(Window::Tab::Chats, [](QWidget *parent, Window::SessionController *controller) {
//           return Ui::Owned<Window::SectionWidget>(new Dialogs::Widget(parent, controller));
//       });
//       Window::RegisterRouteHandler(Window::RouteType::Chat, [](
//               Window::SessionController *controller,
//               const Window::Route &route) {
//           controller->showMainSection(
//               History::Factory(route.id, route.seq()),
//               { .tab = Window::Tab::Chats });
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
// a registered factory shows a placeholder, and so does a route (window_routes.h)
// nobody registered a handler for.
#pragma once

#include "ui/rp_widget.h"
#include "window/window_routes.h"

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
// Returns true if it opened the route. The raw form sees every route string first
// (also the ones outside the canonical table, like "debug/…").
using RouteHandler = std::function<bool(
	SessionController *controller,
	const QString &route)>;
using TypedRouteHandler = std::function<bool(
	SessionController *controller,
	const Route &route)>;

// The side column content of a tab. Registered once at startup.
void RegisterTab(Tab tab, SectionFactory factory);
// The signed-out flow (phone -> code -> password -> profile).
void RegisterIntro(IntroFactory factory);
void RegisterRouteHandler(RouteHandler handler);
// The handler of one kind of canonical route (window_routes.h).
void RegisterRouteHandler(RouteType type, TypedRouteHandler handler);

// The tab a route belongs to; `current` for routes that open where the user is
// (profiles).
[[nodiscard]] Tab RouteTab(const Route &route, Tab current);

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
