#include "window/window_registry.h"

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
	// Modules first: they may refine a tab route ("settings/privacy").
	for (const auto &handler : Data().routes) {
		if (handler(controller, route)) {
			return true;
		}
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
	const auto slash = route.indexOf(QLatin1Char('/'));
	const auto head = (slash < 0) ? route : route.left(slash);
	if (const auto tab = TabFromId(head)) {
		controller->showTab(*tab);
		return true;
	}
	return false;
}

} // namespace Window
