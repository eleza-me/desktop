#include "groups/groups_module.h"

#include "data/data_groups.h"
#include "data/data_session.h"
#include "groups/groups_admin.h"
#include "groups/groups_boxes.h"
#include "groups/groups_info.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "window/window_embeds.h"
#include "window/window_registry.h"
#include "window/window_session_controller.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QCursor>

namespace Groups {
namespace {

#ifdef ELEZA_DEBUG_TOOLS
constexpr auto kDebugRoutePollMs = 100;
constexpr auto kDebugRoutePollCount = 40;

// "debug/…" routes of the module (docs/DEVELOPING.md, section 2). <g> is a
// group id, <u> a user id:
//   debug/group-new                  «گروه تازه»: picking people
//   debug/group-add/<g>              «افزودن عضو»
//   debug/group-edit/<g>             «ویرایش گروه»
//   debug/group-notify/<g>           the notification level
//   debug/group-leave/<g>            the «ترک گروه» confirmation
//   debug/group-clear/<g>            the «پاک کردن تاریخچه» confirmation
//   debug/group-delete/<g>           «حذف گروه»
//   debug/group-report/<g>           the report box of a group
//   debug/group-perms/<g>            «مجوزها»
//   debug/group-links/<g>            «پیوندهای دعوت»
//   debug/group-requests/<g>         «درخواست‌های عضویت»
//   debug/group-members/<g>/<filter> admins | restricted | banned
//   debug/group-admin/<g>/<u>        «مدیر کردن» for a member
//   debug/group-restrict/<g>/<u>     «محدود کردن عضو»
//   debug/group-member-menu/<g>/<u>[/<role>]  the menu of a member row (at the cursor)
//   debug/group-slow/<g>             «حالت آهسته»
//   debug/group-hide/<g>             «پنهان کردن اعضا»
//   debug/group-reactions/<g>/<seq>  who reacted to a message
//   debug/group-link-members/<g>/<link>  who joined through an invite link
bool ShowDebugRoute(
		Window::SessionController *controller,
		const QString &name) {
	const auto parts = name.mid(u"debug/group-"_s.size()).split(QLatin1Char('/'));
	const auto &what = parts[0];
	const auto group = parts.value(1);
	const auto extra = parts.value(2);
	const auto member = [&] {
		auto result = Data::GroupMember();
		result.user = extra;
		result.role = parts.value(3, u"member"_s);
		result.status = u"member"_s;
		return result;
	};
	if (what == u"new") {
		ShowNewGroup(controller);
	} else if (group.isEmpty()) {
		return false;
	} else if (what == u"add") {
		ShowAddMembers(controller, group);
	} else if (what == u"edit") {
		ShowEditBox(controller, group);
	} else if (what == u"notify") {
		ShowNotifyBox(controller, group);
	} else if (what == u"leave") {
		ShowLeaveBox(controller, group);
	} else if (what == u"clear") {
		ShowClearHistoryBox(controller, group);
	} else if (what == u"delete") {
		ShowDeleteGroupBox(controller, group);
	} else if (what == u"report") {
		ShowReportGroup(controller, group);
	} else if (what == u"perms") {
		ShowPermissionsBox(controller, group);
	} else if (what == u"links") {
		ShowInviteLinksBox(controller, group);
	} else if (what == u"requests") {
		ShowJoinRequestsBox(controller, group);
	} else if (what == u"members") {
		ShowMembersBox(controller, group, extra.isEmpty() ? u"admins"_s : extra);
	} else if (what == u"admin" && !extra.isEmpty()) {
		ShowAdminBox(controller, group, member(), nullptr);
	} else if (what == u"restrict" && !extra.isEmpty()) {
		ShowRestrictBox(controller, group, member(), nullptr);
	} else if (what == u"slow") {
		ShowSlowModeBox(controller, group);
	} else if (what == u"hide") {
		ShowHideMembersBox(controller, group);
	} else if (what == u"reactions" && extra.toLongLong() > 0) {
		ShowReactionsBox(controller, group, extra.toLongLong());
	} else if (what == u"link-members" && !extra.isEmpty()) {
		ShowInviteMembersBox(controller, group, extra, QString());
	} else if (what == u"member-menu" && !extra.isEmpty()) {
		ShowMemberMenu(controller, group, member(), QCursor::pos(), nullptr);
	} else {
		return false;
	}
	return true;
}

// The boxes read the group from the store: a route given on the command line
// waits (for a few seconds) until the first list of groups arrived.
void ShowDebugRouteWhenLoaded(
		QPointer<Window::SessionController> controller,
		const QString &name,
		const QString &group,
		int attemptsLeft) {
	if (!controller) {
		return;
	}
	auto &groups = controller->session().data().groups();
	if (groups.group(group) || attemptsLeft <= 0) {
		ShowDebugRoute(controller, name);
		return;
	}
	QTimer::singleShot(kDebugRoutePollMs, controller, [=] {
		ShowDebugRouteWhenLoaded(controller, name, group, attemptsLeft - 1);
	});
}

bool HandleDebugRoute(
		Window::SessionController *controller,
		const QString &route) {
	const auto name = route.startsWith(QLatin1Char('/')) ? route.mid(1) : route;
	if (!name.startsWith(u"debug/group-"_s)) {
		return false;
	}
	const auto group = name.section(QLatin1Char('/'), 2, 2);
	if (group.isEmpty()) {
		return ShowDebugRoute(controller, name);
	}
	controller->session().data().groups().ensureSynced();
	ShowDebugRouteWhenLoaded(controller, name, group, kDebugRoutePollCount);
	return true;
}
#endif // ELEZA_DEBUG_TOOLS

} // namespace

void Register() {
	Window::RegisterRouteHandler(Window::RouteType::NewGroup, [](
			Window::SessionController *controller,
			const Window::Route &) {
		controller->activateTab(Window::Tab::Chats);
		ShowNewGroup(controller);
		return true;
	});
	Window::RegisterRouteHandler(Window::RouteType::Group, [](
			Window::SessionController *controller,
			const Window::Route &route) {
		auto &groups = controller->session().data().groups();
		if (route.id.isEmpty()) {
			return false;
		} else if (!groups.enabled()) {
			// Nothing of groups exists while the server has them off.
			controller->showToast(tr::lng_groups_not_available(tr::now));
			return true;
		}
		const auto show = route.queryValue(u"show"_s);
		if (show == u"reactions") {
			// A box over whatever is on screen (the chat of the group).
			if (route.seq() > 0) {
				ShowReactionsBox(controller, route.id, route.seq());
			}
			return true;
		}
		controller->showMainSection(
			(show == u"media" && groups.thirdRelease())
				? MediaFactory(route.id)
				: (show == u"log" && groups.thirdRelease())
				? AdminLogFactory(route.id)
				: InfoFactory(route.id),
			{ .tab = Window::Tab::Chats });
		return true;
	});
	Window::RegisterRouteHandler(Window::RouteType::GroupInvite, [](
			Window::SessionController *controller,
			const Window::Route &route) {
		if (route.id.isEmpty()) {
			return false;
		}
		// A box over whatever is on screen: the link may come from a chat.
		ShowInvitePreview(controller, route.id);
		return true;
	});
	Window::RegisterChatMenuFiller(FillChatMenu);
#ifdef ELEZA_DEBUG_TOOLS
	Window::RegisterRouteHandler(HandleDebugRoute);
#endif // ELEZA_DEBUG_TOOLS
}

} // namespace Groups
