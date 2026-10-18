// The canonical routes of the app: one string per place the user can be taken to.
//
//   chats                      the chat list tab
//   chat/<chatId>[?seq=N]      a chat (optionally at a message)
//   chat/<chatId>?start=<p>    a chat with a bot opened from its deep link: «شروع» sends "/start <p>"
//   user/<userId>              a profile by user id
//   u/<handle>                 a profile by handle
//   u/<handle>?start=<p>       a bot's deep link (https://eleza.me/<bot>?start=<p>): the chat
//                              with the bot and its «شروع»; a profile when it is not a bot
//   eli                        the feed tab
//   eli/post/<postId>          a post with its replies
//   eli/share/<slug>           a post by its share link (https://eleza.me/s/<slug>)
//   eli/topic/<slug>           a topic (#tag) page
//   eli/notifications          the notifications list
//   search[?q=text]            the search tab
//   me                         own profile tab
//   settings[/<section>]       settings (account, privacy, notifications, …)
//   conversation/topics        the open conversations list
//   conversation/<id>          an anonymous conversation (its sheet / chat)
//   group/new                  «گروه تازه»: pick people, then name the group
//   group/<groupId>            the info of a group (members, settings, administration)
//   group/<groupId>?show=media the shared media of a group (photos, files, links, voice)
//   group/<groupId>?show=log   «اقدام‌های اخیر» (the admin log)
//   group/<groupId>?show=reactions&seq=N   who reacted to a message (a box)
//   g/<token>                  an invite link of a group (https://eleza.me/g/<token>)
//
// The chat of a group is a chat route like any other: chat/<groupId>.
//
// Build them with the Window::Routes helpers, open them with
// controller->openRoute(route). A feature module registers the handler of its
// routes once (Window::RegisterRouteHandler in window_registry.h); until it does,
// opening the route shows a placeholder section, so cross-module navigation is
// always safe to call.
//
// This file has no widget dependencies (the unit tests compile it alone).
#pragma once

#include <QtCore/QMap>
#include <QtCore/QString>

#include <optional>

namespace Window {

enum class RouteType {
	Unknown,
	Chats,
	Chat, // id = chat id; query "seq"
	User, // id = user id
	Handle, // id = handle (without "@")
	Eli,
	Post, // id = post id
	SharedPost, // id = the slug of a share link
	Topic, // id = slug
	EliNotifications,
	Search, // query "q"
	Me,
	Settings, // id = section ("" for the root)
	ConversationTopics,
	Conversation, // id = conversation id
	NewGroup,
	Group, // id = group id
	GroupInvite, // id = the token of an invite link
};

struct Route {
	RouteType type = RouteType::Unknown;
	QString id;
	QMap<QString, QString> query;

	[[nodiscard]] bool valid() const {
		return type != RouteType::Unknown;
	}
	// The message to scroll to in a chat route, 0 if none.
	[[nodiscard]] qint64 seq() const;
	// A bot's deep link: the ?start= payload (maybe empty), nullopt if none.
	[[nodiscard]] std::optional<QString> botStart() const;
	[[nodiscard]] QString queryValue(const QString &name) const {
		return query.value(name);
	}
	// The canonical string ("chat/c1?seq=12").
	[[nodiscard]] QString toString() const;

	friend bool operator==(const Route &a, const Route &b) {
		return (a.type == b.type) && (a.id == b.id) && (a.query == b.query);
	}
};

// Accepts the canonical form with or without a leading "/".
[[nodiscard]] Route ParseRoute(const QString &route);

// A link to the web app (https://eleza.me/chats/c1, /u/sara, /@sara,
// /eli/post/p1, /p/p1, /s/<share slug>, /g/<invite token>, /eli/topic/x…) as a
// route; a bare /<handle> is a profile (a page of the site — /terms, /faq… —
// is not) and keeps a bot's ?start= payload; nullopt for
// a path the desktop app has no screen for (the caller opens the browser then).
[[nodiscard]] std::optional<Route> RouteFromWebPath(
	const QString &path,
	const QString &query = QString());

namespace Routes {

[[nodiscard]] QString Chats();
[[nodiscard]] QString Chat(const QString &chatId, qint64 seq = 0);
[[nodiscard]] QString User(const QString &userId);
[[nodiscard]] QString Handle(const QString &handle);
// A bot's deep link as a route (u/<handle>?start=<payload>).
[[nodiscard]] QString HandleStart(const QString &handle, const QString &payload);
// The chat with a bot, «شروع» shown with the payload of a deep link.
[[nodiscard]] QString BotStart(const QString &chatId, const QString &payload);
[[nodiscard]] QString Eli();
[[nodiscard]] QString Post(const QString &postId);
[[nodiscard]] QString SharedPost(const QString &slug);
[[nodiscard]] QString Topic(const QString &slug);
[[nodiscard]] QString EliNotifications();
[[nodiscard]] QString Search(const QString &query = QString());
[[nodiscard]] QString Me();
[[nodiscard]] QString Settings(const QString &section = QString());
[[nodiscard]] QString ConversationTopics();
[[nodiscard]] QString Conversation(const QString &id);
[[nodiscard]] QString NewGroup();
[[nodiscard]] QString Group(const QString &groupId);
[[nodiscard]] QString GroupMedia(const QString &groupId);
[[nodiscard]] QString GroupAdminLog(const QString &groupId);
[[nodiscard]] QString GroupReactions(const QString &groupId, qint64 seq);
[[nodiscard]] QString GroupInvite(const QString &token);

} // namespace Routes
} // namespace Window
