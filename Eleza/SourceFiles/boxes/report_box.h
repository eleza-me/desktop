// Boxes::ShowReportBox — «گزارش»: the reasons the server offers for this kind
// of target, an optional note and, for content with a known author, «این
// کاربر را هم مسدود کن». One form for posts, replies, accounts and
// conversation topics, groups and messages of groups (they all go to
// POST /v1/reports).
//
//   auto args = Boxes::ReportArgs();
//   args.kind = u"post"_s;
//   args.id = postId;
//   args.offerBlockUserId = authorId;
//   Boxes::ShowReportBox(controller, std::move(args));
#pragma once

#include <QtCore/QString>

#include <functional>

namespace Window {
class SessionController;
} // namespace Window

namespace Boxes {

struct ReportArgs {
	QString kind; // post | reply | account | conversation | group | group_message
	QString id;   // (a group or a message of one: the group's id)
	qint64 messageSeq = 0; // group_message: which message
	// Offers «also block» for this user (never for myself or a conversation).
	QString offerBlockUserId;
	// The server took the report (the box closes by itself).
	std::function<void()> sent;
	// «also block» was checked and the user is blocked now.
	std::function<void()> blocked;
};

void ShowReportBox(Window::SessionController *controller, ReportArgs args);

} // namespace Boxes
