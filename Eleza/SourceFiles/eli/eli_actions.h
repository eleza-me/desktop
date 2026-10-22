// What can be done with a post from anywhere it is shown: the «…» menu, the
// right-click menu, the repost and share menus, like / save toggles, delete,
// block, reply policy — and the entry points of the other «الی» screens
// (compose, viewers, boxes, pages), so cards and sections need one header.
#pragma once

#include "data/data_social_types.h"
#include "window/window_registry.h"

#include <QtCore/QPoint>

namespace Window {
class SessionController;
} // namespace Window

namespace Eli {

// ---- people
void OpenProfile(Window::SessionController *controller, const QString &userId);
void ConfirmBlock(Window::SessionController *controller, const QString &userId);

// ---- one-click actions (optimistic; a failure shows a toast)
void ToggleLike(Window::SessionController *controller, const QString &postId);
void ToggleDislike(Window::SessionController *controller, const QString &postId);
void ToggleBookmark(Window::SessionController *controller, const QString &postId);
void ToggleRepost(Window::SessionController *controller, const QString &postId);
// Makes the share link and copies it.
void CopyPostLink(Window::SessionController *controller, const QString &postId);
// «ارسال در گفتگو»: pick chats, the post goes there as a card.
void SendPostToChats(Window::SessionController *controller, const QString &postId);
void ConfirmDeletePost(Window::SessionController *controller, const QString &postId);
// A reply of mine was just published: the posts above it count one more
// (the server tells with the next payload; the cards should not lag).
void BumpReplies(Window::SessionController *controller, const Data::Post &created);

// ---- menus (popups at a global position)
// The «…» button of a card.
void ShowMoreMenu(
	Window::SessionController *controller,
	QWidget *parent,
	const QString &postId,
	QPoint globalPosition);
// Right click on a card: the quick actions first, then everything of «…».
void ShowContextMenu(
	Window::SessionController *controller,
	QWidget *parent,
	const QString &postId,
	QPoint globalPosition,
	const QString &linkToCopy = QString());
void ShowRepostMenu(
	Window::SessionController *controller,
	QWidget *parent,
	const QString &postId,
	QPoint globalPosition);
void ShowShareMenu(
	Window::SessionController *controller,
	QWidget *parent,
	const QString &postId,
	QPoint globalPosition);
// A reply row under a post: copy, report, delete (mine or under my post).
void ShowReplyMenu(
	Window::SessionController *controller,
	QWidget *parent,
	const QString &rootId,
	const QString &replyId,
	QPoint globalPosition);

// ---- compose (eli_compose_box.cpp)
struct ComposeArgs {
	enum class Kind {
		New,
		Reply,
		Quote,
		Invite,
		Edit,
	};
	Kind kind = Kind::New;
	QString postId; // the post replied to / quoted / edited
	QString conversationId; // Invite
	QString prefill;
	QStringList files; // attached at once (dropped on the feed)
};
void OpenCompose(Window::SessionController *controller, ComposeArgs args = {});

// ---- viewers (eli_viewer.cpp, eli_pardeh.cpp)
// The images of the post (or of the post it quotes) in the full-window viewer.
void OpenImageViewer(
	Window::SessionController *controller,
	const Data::Post &post,
	int index);
// The round video played large over the dimmed, blurred window.
void OpenVideoNote(
	Window::SessionController *controller,
	const QString &postId);
// «پرده»: the vertical video pager starting at the post; with `authorId` the
// next pages are that author's videos, otherwise the video feed.
void OpenPardeh(
	Window::SessionController *controller,
	const QString &postId,
	const QString &authorId = QString());

// ---- boxes
void OpenReactors(
	Window::SessionController *controller,
	const QString &postId,
	bool dislikes);
struct ReportTarget {
	QString kind; // post | reply | account | conversation
	QString id;
	QString offerBlockUserId;
};
void OpenReport(Window::SessionController *controller, ReportTarget target);

// ---- pages (main column sections)
// `rootHint`: the post at the top of the conversation of a comment, as a
// notification named it (read together with the comment).
[[nodiscard]] Window::SectionFactory PostFactory(
	const QString &postId,
	const QString &rootHint = QString());
[[nodiscard]] Window::SectionFactory ThreadFactory(const QString &postId);
[[nodiscard]] Window::SectionFactory NotificationsFactory();
[[nodiscard]] Window::SectionFactory FollowRequestsFactory();
[[nodiscard]] Window::SectionFactory TopicFactory(const QString &slug);
[[nodiscard]] Window::SectionFactory TrendsFactory();
[[nodiscard]] Window::SectionFactory SoundFactory(const QString &soundId);
[[nodiscard]] Window::SectionFactory BookmarksFactory();

void OpenPost(Window::SessionController *controller, const QString &postId);
void OpenThread(Window::SessionController *controller, const QString &postId);
void OpenTrends(Window::SessionController *controller);
void OpenBookmarks(Window::SessionController *controller);
void OpenSound(Window::SessionController *controller, const QString &soundId);
void OpenFollowRequests(Window::SessionController *controller);

// Routes of ours that are not in the canonical table (window_routes.h).
namespace Routes {
[[nodiscard]] QString Thread(const QString &postId); // eli/thread/<id>
[[nodiscard]] QString Trends(); // eli/trends
[[nodiscard]] QString Bookmarks(); // eli/bookmarks
[[nodiscard]] QString Sound(const QString &soundId); // eli/sound/<id>
[[nodiscard]] QString FollowRequests(); // eli/notifications/requests
[[nodiscard]] QString Compose(); // eli/compose
} // namespace Routes

} // namespace Eli
