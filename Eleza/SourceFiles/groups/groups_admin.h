// The third release of groups (a server that lists `groups.admin_log.page_size`):
// slow mode, hidden members, recent actions (the admin log), the shared media
// of a group by kind, who reacted to a message, and the people who joined
// through one invite link.
//
//   Groups::ShowSlowModeBox(controller, groupId);          // «حالت آهسته» (ban_users)
//   Groups::ShowHideMembersBox(controller, groupId);       // «پنهان کردن اعضا» (ban_users)
//   controller->openRoute(Window::Routes::GroupAdminLog(groupId));   // owner and admins
//   controller->openRoute(Window::Routes::GroupMedia(groupId));
//   controller->openRoute(Window::Routes::GroupReactions(groupId, seq));
//
// Every list here is paged from the server; nothing is polled (the admin log
// is not live: it is read when the screen opens and on «تلاش دوباره»).
#pragma once

#include "window/window_registry.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Groups {

// «۳۰ ثانیه», «۵ دقیقه», «۱ ساعت»; «خاموش» for 0.
[[nodiscard]] QString SlowModeLabel(int seconds);

void ShowSlowModeBox(
	Window::SessionController *controller,
	const QString &groupId);
void ShowHideMembersBox(
	Window::SessionController *controller,
	const QString &groupId);

// Who reacted to the message `seq` of a group, with what.
void ShowReactionsBox(
	Window::SessionController *controller,
	const QString &groupId,
	qint64 seq);

// The people who joined through an invite link and are members now.
void ShowInviteMembersBox(
	Window::SessionController *controller,
	const QString &groupId,
	const QString &link,
	const QString &title);

// The sections (routes group/<id>?show=log, group/<id>?show=media).
[[nodiscard]] Window::SectionFactory AdminLogFactory(const QString &groupId);
[[nodiscard]] Window::SectionFactory MediaFactory(const QString &groupId);

} // namespace Groups
