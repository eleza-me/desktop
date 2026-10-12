// The boxes and menus of groups. Everything asks Data::Groups; what a
// screen may offer is decided by Data::GroupRights and by the permissions /
// rights the group's header lists for me (the server has the last word, and
// its refusal — `group.right_required` — is shown like any other error).
//
//   Groups::ShowNewGroup(controller);                  // pick people, name the group, open it
//   Groups::ShowInvitePreview(controller, token);      // https://eleza.me/g/<token>
//   Groups::ShowMemberMenu(controller, groupId, member, globalPosition, changed);
#pragma once

#include "data/data_types.h"

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Groups {

// «گروه تازه»: the people, then the name.
void ShowNewGroup(Window::SessionController *controller);
void ShowAddMembers(Window::SessionController *controller, const QString &groupId);
// What an invite link leads to: join, ask to join, or open the group.
void ShowInvitePreview(Window::SessionController *controller, const QString &token);

void ShowLeaveBox(Window::SessionController *controller, const QString &groupId);
void ShowClearHistoryBox(Window::SessionController *controller, const QString &groupId);
void ShowNotifyBox(Window::SessionController *controller, const QString &groupId);
void ShowEditBox(Window::SessionController *controller, const QString &groupId);
void ShowDeleteGroupBox(Window::SessionController *controller, const QString &groupId);
void ShowReportGroup(Window::SessionController *controller, const QString &groupId);

// ---- administration (a server with the second release of groups)
void ShowPermissionsBox(Window::SessionController *controller, const QString &groupId);
void ShowInviteLinksBox(Window::SessionController *controller, const QString &groupId);
void ShowJoinRequestsBox(Window::SessionController *controller, const QString &groupId);
// filter: "admins" | "restricted" | "banned".
void ShowMembersBox(
	Window::SessionController *controller,
	const QString &groupId,
	const QString &filter);
void ShowAdminBox(
	Window::SessionController *controller,
	const QString &groupId,
	const Data::GroupMember &member,
	Fn<void()> changed);
void ShowRestrictBox(
	Window::SessionController *controller,
	const QString &groupId,
	const Data::GroupMember &member,
	Fn<void()> changed);

// What I may do to a member (profile, make admin, restrict, remove, ban,
// transfer…); `changed` is called after something was done.
void ShowMemberMenu(
	Window::SessionController *controller,
	const QString &groupId,
	const Data::GroupMember &member,
	QPoint globalPosition,
	Fn<void()> changed);

// «اطلاعات گروه», «پاک کردن تاریخچه», «ترک گروه» for the menu of a chat row
// and the «…» menu of the chat screen (nothing for a chat that is not a group).
void FillChatMenu(
	Ui::PopupMenu *menu,
	Window::SessionController *controller,
	const QString &chatId);

// «مالک» / «مدیر» (or the admin's own title) / «محدودشده»; empty for a member.
[[nodiscard]] QString MemberTag(const Data::GroupMember &member);
// The words of a right / a permission of the wire lists.
[[nodiscard]] QString RightLabel(const QString &name);
[[nodiscard]] QString PermissionLabel(const QString &name);

} // namespace Groups
