#include "profile/profile_module.h"

#include "data/data_session.h"
#include "data/data_users.h"
#include "main/main_session.h"
#include "profile/profile_boxes.h"
#include "profile/profile_edit.h"
#include "profile/profile_follow_list.h"
#include "profile/profile_side.h"
#include "profile/profile_widget.h"
#include "window/window_registry.h"
#include "window/window_session_controller.h"

namespace Profile {
namespace {

[[nodiscard]] bool OpenOwnRoute(
		Window::SessionController *controller,
		const QString &route) {
	auto options = Window::SectionShow();
	options.tab = Window::Tab::Me;
	if (route == u"me/profile" || route == u"me/bookmarks") {
		options.stack = false;
		controller->showMainSection(
			Factory(
				Source::Me(),
				(route == u"me/bookmarks")
					? PostsTab::Bookmarks
					: PostsTab::Posts),
			options);
		return true;
	} else if (route == u"me/edit") {
		controller->showMainSection(EditFactory(), options);
		return true;
	} else if (route == u"me/requests") {
		controller->showMainSection(FollowRequestsFactory(), options);
		return true;
	}
	return false;
}

// "u/<handle>/followers", "user/<id>/following".
[[nodiscard]] bool OpenFollowRoute(
		Window::SessionController *controller,
		const QString &route) {
	const auto parts = route.split(QLatin1Char('/'));
	if (parts.size() != 3
		|| parts[1].isEmpty()
		|| (parts[0] != u"u" && parts[0] != u"user")
		|| (parts[2] != u"followers" && parts[2] != u"following")) {
		return false;
	}
	auto profile = Data::Profile();
	if (parts[0] == u"u") {
		profile.handle = parts[1];
	} else {
		profile.id = parts[1];
	}
	controller->showMainSection(FollowListFactory(
		profile,
		(parts[2] == u"followers")
			? FollowKind::Followers
			: FollowKind::Following));
	return true;
}

[[nodiscard]] bool OpenDebugRoute(
		Window::SessionController *controller,
		const QString &route) {
	if (route == u"debug/birthday" || route == u"debug/birthday-empty") {
		auto current = std::optional<Data::Birthday>();
		if (route == u"debug/birthday") {
			// Esfand 30 of a leap year: the day list shrinks with the year.
			auto sample = Data::Birthday();
			sample.calendar = u"jalali"_s;
			sample.year = 1375;
			sample.month = 12;
			sample.day = 30;
			current = sample;
		}
		controller->show(Ui::Box<BirthdayBox>(
			current,
			[](std::optional<Data::Birthday>) {}));
		return true;
	} else if (route == u"debug/share") {
		// The route may be opened before my profile arrived.
		controller->session().data().users().refreshMe(
			controller,
			[=](const Data::Profile &profile) {
				ShowShareBox(controller, profile);
			});
		return true;
	}
	return false;
}

} // namespace

void Register() {
	Window::RegisterTab(Window::Tab::Me, [](
			QWidget *parent,
			Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new SideWidget(parent, controller));
	});
	Window::RegisterRouteHandler([](
			Window::SessionController *controller,
			const QString &route) {
		const auto clean = route.startsWith(QLatin1Char('/'))
			? route.mid(1)
			: route;
		if (clean.startsWith(u"me/")) {
			return OpenOwnRoute(controller, clean);
		} else if (clean.startsWith(u"debug/")) {
			return OpenDebugRoute(controller, clean);
		}
		return OpenFollowRoute(controller, clean);
	});
	Window::RegisterRouteHandler(Window::RouteType::User, [](
			Window::SessionController *controller,
			const Window::Route &route) {
		// Profiles open where the user is (the current tab's stack).
		controller->showMainSection(Factory(Source::UserId(route.id)));
		return true;
	});
	Window::RegisterRouteHandler(Window::RouteType::Handle, [](
			Window::SessionController *controller,
			const Window::Route &route) {
		controller->showMainSection(Factory(Source::Handle(route.id)));
		return true;
	});
	Window::RegisterRouteHandler(Window::RouteType::Me, [](
			Window::SessionController *controller,
			const Window::Route &) {
		// The side column of the tab opens the full profile next to itself
		// when there is room for two columns.
		controller->showTab(Window::Tab::Me);
		return true;
	});
}

} // namespace Profile
