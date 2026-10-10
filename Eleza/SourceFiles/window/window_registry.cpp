#include "window/window_registry.h"

#include "window/window_main_window.h"
#include "window/window_placeholder_section.h"
#include "window/window_section_widget.h"
#include "window/window_session_controller.h"

#include <algorithm>
#include <array>
#include <vector>

namespace Window {
namespace {

struct Registry {
	std::array<SectionFactory, kTabsCount> tabs;
	IntroFactory intro;
	std::vector<RouteHandler> routes;
	std::vector<std::pair<RouteType, TypedRouteHandler>> typedRoutes;
};

[[nodiscard]] Registry &Data() {
	static auto result = Registry();
	return result;
}

} // namespace

QString TabId(Tab tab) {
	switch (tab) {
	case Tab::Eli: return QStringLiteral("eli");
	case Tab::Chats: return QStringLiteral("chats");
	case Tab::Search: return QStringLiteral("search");
	case Tab::Me: return QStringLiteral("me");
	case Tab::Settings: return QStringLiteral("settings");
	}
	return QString();
}

std::optional<Tab> TabFromId(const QString &id) {
	for (auto i = 0; i != kTabsCount; ++i) {
		if (TabId(Tab(i)) == id) {
			return Tab(i);
		}
	}
	return std::nullopt;
}

void RegisterTab(Tab tab, SectionFactory factory) {
	Data().tabs[int(tab)] = std::move(factory);
}

void RegisterIntro(IntroFactory factory) {
	Data().intro = std::move(factory);
}

void RegisterRouteHandler(RouteHandler handler) {
	Data().routes.push_back(std::move(handler));
}

void RegisterRouteHandler(RouteType type, TypedRouteHandler handler) {
	Data().typedRoutes.emplace_back(type, std::move(handler));
}

Tab RouteTab(const Route &route, Tab current) {
	switch (route.type) {
	case RouteType::Chats:
	case RouteType::Chat:
	case RouteType::ConversationTopics:
	case RouteType::Conversation: return Tab::Chats;
	case RouteType::Eli:
	case RouteType::Post:
	case RouteType::SharedPost:
	case RouteType::Topic:
	case RouteType::EliNotifications: return Tab::Eli;
	case RouteType::Search: return Tab::Search;
	case RouteType::Me: return Tab::Me;
	case RouteType::Settings: return Tab::Settings;
	case RouteType::User:
	case RouteType::Handle:
	case RouteType::Unknown: break;
	}
	return current;
}

SectionFactory RegisteredTabFactory(Tab tab) {
	return Data().tabs[int(tab)];
}

bool HasIntro() {
	return Data().intro != nullptr;
}

Ui::object_ptr<Ui::RpWidget> CreateIntro(QWidget *parent, MainWindow *window) {
	const auto &factory = Data().intro;
	return factory ? factory(parent, window) : Ui::object_ptr<Ui::RpWidget>();
}

bool HandleRoute(SessionController *controller, const QString &route) {
	if (route.isEmpty()) {
		return false;
	}
#ifndef ELEZA_DEBUG_TOOLS
	// A build without the debug tools has no developer routes at all; whatever
	// produced one (a link, a stored state) opens nothing.
	if (route.startsWith(QStringLiteral("debug/"))
		|| route.startsWith(QStringLiteral("/debug/"))) {
		return false;
	}
#endif // !ELEZA_DEBUG_TOOLS
	// Modules first: they may refine a tab route ("settings/privacy").
	for (const auto &handler : Data().routes) {
		if (handler(controller, route)) {
			return true;
		}
	}
	const auto parsed = ParseRoute(route);
	if (parsed.valid()) {
		for (const auto &[type, handler] : Data().typedRoutes) {
			if (type == parsed.type && handler(controller, parsed)) {
				return true;
			}
		}
	}
#ifdef ELEZA_DEBUG_TOOLS
	// "debug/title[/<states>]": what the custom title bar shows (inactive,
	// maximized, dark, hover-close, press-max…); no states: back to normal.
	const auto titlePrefix = QStringLiteral("debug/title");
	if (route == titlePrefix
		|| route.startsWith(titlePrefix + QLatin1Char('/'))) {
		controller->window()->setDebugTitleState(
			route.mid(titlePrefix.size() + 1));
		return true;
	}
	// "debug/main[/<n>]": n placeholder sections in the main column.
	const auto debugPrefix = QStringLiteral("debug/main");
	if (route.startsWith(debugPrefix)) {
		const auto count = std::clamp(route.mid(debugPrefix.size() + 1).toInt(), 1, 8);
		for (auto i = 0; i != count; ++i) {
			controller->showMainSection(
				PlaceholderMainSection::Factory(i + 1),
				{ .animated = anim::type::instant });
		}
		return true;
	}
#endif // ELEZA_DEBUG_TOOLS
	if (!parsed.valid()) {
		return false;
	}
	// Nobody took it yet: the tab, and a stand-in for what opens in the main
	// column.
	const auto tab = RouteTab(parsed, controller->currentTab());
	switch (parsed.type) {
	case RouteType::Chats:
	case RouteType::Eli:
	case RouteType::Search:
	case RouteType::Me:
		controller->showTab(tab);
		return true;
	case RouteType::Settings:
		controller->showTab(tab);
		if (parsed.id.isEmpty()) {
			return true;
		}
		break;
	default: break;
	}
	controller->showMainSection(
		RoutePlaceholderSection::Factory(parsed),
		{ .tab = tab });
	return true;
}

} // namespace Window
