#include "window/window_routes.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

namespace Window {
namespace {

[[nodiscard]] QString Encode(const QString &value) {
	return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

[[nodiscard]] QString Decode(const QString &value) {
	return QUrl::fromPercentEncoding(value.toUtf8());
}

[[nodiscard]] QMap<QString, QString> ParseQuery(const QString &query) {
	auto result = QMap<QString, QString>();
	const auto parsed = QUrlQuery(query);
	const auto items = parsed.queryItems(QUrl::FullyDecoded);
	for (const auto &[name, value] : items) {
		if (!name.isEmpty()) {
			// "+" means a space in what browsers produce.
			auto decoded = value;
			decoded.replace(QLatin1Char('+'), QLatin1Char(' '));
			result.insert(name, decoded);
		}
	}
	return result;
}

const auto kStart = QStringLiteral("start");

// First path segments of eleza.me that are never a handle: pages of the site
// and the link kinds of the apps.
[[nodiscard]] bool SitePage(const QString &segment) {
	static const auto kPages = QStringList{
		QStringLiteral("terms"), QStringLiteral("privacy"), QStringLiteral("download"),
		QStringLiteral("help"), QStringLiteral("about"), QStringLiteral("support"),
		QStringLiteral("contact"), QStringLiteral("faq"), QStringLiteral("rules"),
		QStringLiteral("blog"), QStringLiteral("press"), QStringLiteral("jobs"),
		QStringLiteral("app"), QStringLiteral("apps"), QStringLiteral("android"),
		QStringLiteral("ios"), QStringLiteral("legal"), QStringLiteral("security"),
		QStringLiteral("status"), QStringLiteral("index.html"), QStringLiteral("robots.txt"),
		QStringLiteral("favicon.ico"), QStringLiteral(".well-known"),
		QStringLiteral("guidelines"), QStringLiteral("transparency"),
		QStringLiteral("copyright"), QStringLiteral("bots"), QStringLiteral("delete-account"),
		QStringLiteral("fa"), QStringLiteral("en"), QStringLiteral("tr"), QStringLiteral("fr"),
		QStringLiteral("de"), QStringLiteral("ar"), QStringLiteral("_nuxt"),
		QStringLiteral("addstickers"), QStringLiteral("tag"), QStringLiteral("c"),
		QStringLiteral("i"), QStringLiteral("p"), QStringLiteral("s"), QStringLiteral("g"),
		QStringLiteral("u"), QStringLiteral("chats"), QStringLiteral("eli"),
		QStringLiteral("search"), QStringLiteral("me"), QStringLiteral("settings"),
		QStringLiteral("login"), QStringLiteral("signup"), QStringLiteral("invite"),
	};
	return kPages.contains(segment.toLower());
}

[[nodiscard]] bool LooksLikeHandle(const QString &text) {
	static const auto kHandle = QRegularExpression(
		QStringLiteral("^@?[A-Za-z0-9_.]{2,32}$"));
	return kHandle.match(text).hasMatch();
}

[[nodiscard]] Route Make(RouteType type, const QString &id = QString()) {
	auto result = Route();
	result.type = type;
	result.id = id;
	return result;
}

[[nodiscard]] QString TypePath(const Route &route) {
	const auto id = Encode(route.id);
	switch (route.type) {
	case RouteType::Chats: return QStringLiteral("chats");
	case RouteType::Chat: return QStringLiteral("chat/") + id;
	case RouteType::User: return QStringLiteral("user/") + id;
	case RouteType::Handle: return QStringLiteral("u/") + id;
	case RouteType::Eli: return QStringLiteral("eli");
	case RouteType::Post: return QStringLiteral("eli/post/") + id;
	case RouteType::SharedPost: return QStringLiteral("eli/share/") + id;
	case RouteType::Topic: return QStringLiteral("eli/topic/") + id;
	case RouteType::EliNotifications:
		return QStringLiteral("eli/notifications");
	case RouteType::Search: return QStringLiteral("search");
	case RouteType::Me: return QStringLiteral("me");
	case RouteType::Settings:
		return route.id.isEmpty()
			? QStringLiteral("settings")
			: (QStringLiteral("settings/") + id);
	case RouteType::ConversationTopics:
		return QStringLiteral("conversation/topics");
	case RouteType::Conversation: return QStringLiteral("conversation/") + id;
	case RouteType::NewGroup: return QStringLiteral("group/new");
	case RouteType::Group: return QStringLiteral("group/") + id;
	case RouteType::GroupInvite: return QStringLiteral("g/") + id;
	case RouteType::Unknown: break;
	}
	return QString();
}

} // namespace

qint64 Route::seq() const {
	const auto value = query.value(QStringLiteral("seq")).toLongLong();
	return (value > 0) ? value : 0;
}

std::optional<QString> Route::botStart() const {
	const auto i = query.constFind(kStart);
	if (i == query.cend()
		|| (type != RouteType::Chat && type != RouteType::Handle)) {
		return std::nullopt;
	}
	return *i;
}

QString Route::toString() const {
	auto result = TypePath(*this);
	if (result.isEmpty() || query.isEmpty()) {
		return result;
	}
	auto parts = QStringList();
	for (auto i = query.begin(); i != query.end(); ++i) {
		parts.push_back(Encode(i.key()) + QLatin1Char('=') + Encode(i.value()));
	}
	return result + QLatin1Char('?') + parts.join(QLatin1Char('&'));
}

Route ParseRoute(const QString &route) {
	auto text = route.trimmed();
	while (text.startsWith(QLatin1Char('/'))) {
		text.remove(0, 1);
	}
	const auto mark = text.indexOf(QLatin1Char('?'));
	const auto query = (mark < 0) ? QString() : text.mid(mark + 1);
	auto path = (mark < 0) ? text : text.left(mark);
	while (path.endsWith(QLatin1Char('/'))) {
		path.chop(1);
	}
	auto parts = path.split(QLatin1Char('/'));
	for (auto &part : parts) {
		part = Decode(part);
	}
	const auto count = parts.size();
	const auto &head = parts[0];
	auto result = Route();
	if (head == QStringLiteral("chats") && count == 1) {
		result = Make(RouteType::Chats);
	} else if (head == QStringLiteral("chat") && count == 2) {
		result = Make(RouteType::Chat, parts[1]);
	} else if (head == QStringLiteral("user") && count == 2) {
		result = Make(RouteType::User, parts[1]);
	} else if (head == QStringLiteral("u") && count == 2) {
		auto handle = parts[1];
		if (handle.startsWith(QLatin1Char('@'))) {
			handle.remove(0, 1);
		}
		result = Make(RouteType::Handle, handle.toLower());
	} else if (head == QStringLiteral("eli")) {
		if (count == 1) {
			result = Make(RouteType::Eli);
		} else if (count == 2 && parts[1] == QStringLiteral("notifications")) {
			result = Make(RouteType::EliNotifications);
		} else if (count == 3 && parts[1] == QStringLiteral("post")) {
			result = Make(RouteType::Post, parts[2]);
		} else if (count == 3 && parts[1] == QStringLiteral("share")) {
			result = Make(RouteType::SharedPost, parts[2]);
		} else if (count == 3 && parts[1] == QStringLiteral("topic")) {
			auto slug = parts[2];
			if (slug.startsWith(QLatin1Char('#'))) {
				slug.remove(0, 1);
			}
			result = Make(RouteType::Topic, slug);
		}
	} else if (head == QStringLiteral("search") && count == 1) {
		result = Make(RouteType::Search);
	} else if (head == QStringLiteral("me") && count == 1) {
		result = Make(RouteType::Me);
	} else if (head == QStringLiteral("settings") && count <= 2) {
		result = Make(RouteType::Settings, (count == 2) ? parts[1] : QString());
	} else if (head == QStringLiteral("conversation") && count == 2) {
		result = (parts[1] == QStringLiteral("topics"))
			? Make(RouteType::ConversationTopics)
			: Make(RouteType::Conversation, parts[1]);
	} else if (head == QStringLiteral("group") && count == 2) {
		result = (parts[1] == QStringLiteral("new"))
			? Make(RouteType::NewGroup)
			: Make(RouteType::Group, parts[1]);
	} else if (head == QStringLiteral("g") && count == 2) {
		result = Make(RouteType::GroupInvite, parts[1]);
	}
	const auto needsId = (result.type == RouteType::Chat)
		|| (result.type == RouteType::User)
		|| (result.type == RouteType::Handle)
		|| (result.type == RouteType::Post)
		|| (result.type == RouteType::SharedPost)
		|| (result.type == RouteType::Topic)
		|| (result.type == RouteType::Conversation)
		|| (result.type == RouteType::Group)
		|| (result.type == RouteType::GroupInvite);
	if (needsId && result.id.isEmpty()) {
		return Route();
	}
	if (result.valid() && !query.isEmpty()) {
		result.query = ParseQuery(query);
	}
	return result;
}

std::optional<Route> RouteFromWebPath(
		const QString &path,
		const QString &query) {
	auto trimmed = path;
	while (trimmed.startsWith(QLatin1Char('/'))) {
		trimmed.remove(0, 1);
	}
	while (trimmed.endsWith(QLatin1Char('/'))) {
		trimmed.chop(1);
	}
	auto parts = trimmed.isEmpty()
		? QStringList()
		: trimmed.split(QLatin1Char('/'));
	for (auto &part : parts) {
		part = Decode(part);
	}
	const auto count = parts.size();
	auto result = Route();
	if (count == 0) {
		result = Make(RouteType::Eli);
	} else if (parts[0] == QStringLiteral("chats")) {
		// The web app's lists ("/chats/archived", "/chats/requests") live
		// inside the chats tab here.
		if (count == 1
			|| parts[1] == QStringLiteral("archived")
			|| parts[1] == QStringLiteral("requests")) {
			result = Make(RouteType::Chats);
		} else if (count == 2) {
			result = Make(RouteType::Chat, parts[1]);
		}
	} else if (parts[0] == QStringLiteral("u") && count >= 2) {
		// "/u/<handle>/followers" opens the profile.
		result = ParseRoute(QStringLiteral("u/") + Encode(parts[1]));
	} else if (parts[0].startsWith(QLatin1Char('@')) && count == 1) {
		// The public address of a profile: "/@sara".
		result = ParseRoute(QStringLiteral("u/") + Encode(parts[0].mid(1)));
	} else if (count == 1 && !SitePage(parts[0]) && LooksLikeHandle(parts[0])) {
		// "/sara", and a bot's deep link "/weather_bot?start=promo".
		result = ParseRoute(QStringLiteral("u/") + Encode(parts[0]));
	} else if (parts[0] == QStringLiteral("p") && count == 2) {
		// The public address of a post.
		result = Make(RouteType::Post, parts[1]);
	} else if (parts[0] == QStringLiteral("s") && count == 2) {
		result = Make(RouteType::SharedPost, parts[1]);
	} else if (parts[0] == QStringLiteral("g") && count == 2 && !parts[1].isEmpty()) {
		// An invite link of a group.
		result = Make(RouteType::GroupInvite, parts[1]);
	} else if (parts[0] == QStringLiteral("eli")) {
		if (count == 3 && parts[1] == QStringLiteral("thread")) {
			result = Make(RouteType::Post, parts[2]);
		} else if (count >= 2 && parts[1] == QStringLiteral("notifications")) {
			result = Make(RouteType::EliNotifications);
		} else if (count == 2
			&& (parts[1] == QStringLiteral("trends")
				|| parts[1] == QStringLiteral("bookmarks"))) {
			result = Make(RouteType::Eli);
		} else {
			result = ParseRoute(trimmed);
		}
	} else if (parts[0] == QStringLiteral("search")
		|| parts[0] == QStringLiteral("me")
		|| parts[0] == QStringLiteral("settings")) {
		result = ParseRoute(trimmed);
	}
	if (!result.valid()) {
		return std::nullopt;
	}
	if (!query.isEmpty()) {
		auto parsed = ParseQuery(query);
		if (result.type == RouteType::Chat && parsed.contains(QStringLiteral("seq"))) {
			result.query.insert(
				QStringLiteral("seq"),
				parsed.value(QStringLiteral("seq")));
		} else if (result.type == RouteType::Handle && parsed.contains(kStart)) {
			result.query.insert(kStart, parsed.value(kStart));
		} else if (result.type == RouteType::Search
			&& parsed.contains(QStringLiteral("q"))) {
			result.query.insert(
				QStringLiteral("q"),
				parsed.value(QStringLiteral("q")));
		}
	}
	return result;
}

namespace Routes {

QString Chats() {
	return Make(RouteType::Chats).toString();
}

QString Chat(const QString &chatId, qint64 seq) {
	auto route = Make(RouteType::Chat, chatId);
	if (seq > 0) {
		route.query.insert(QStringLiteral("seq"), QString::number(seq));
	}
	return route.toString();
}

QString User(const QString &userId) {
	return Make(RouteType::User, userId).toString();
}

QString Handle(const QString &handle) {
	auto clean = handle;
	if (clean.startsWith(QLatin1Char('@'))) {
		clean.remove(0, 1);
	}
	return Make(RouteType::Handle, clean.toLower()).toString();
}

QString HandleStart(const QString &handle, const QString &payload) {
	auto route = ParseRoute(Handle(handle));
	if (!route.valid()) {
		return QString();
	}
	route.query.insert(kStart, payload);
	return route.toString();
}

QString BotStart(const QString &chatId, const QString &payload) {
	auto route = Make(RouteType::Chat, chatId);
	route.query.insert(kStart, payload);
	return route.toString();
}

QString ChatMedia(const QString &chatId) {
	auto result = Make(RouteType::Chat, chatId);
	result.query.insert(QStringLiteral("show"), QStringLiteral("media"));
	return result.toString();
}

QString Eli() {
	return Make(RouteType::Eli).toString();
}

QString Post(const QString &postId, const QString &rootHint) {
	auto route = Make(RouteType::Post, postId);
	if (!rootHint.isEmpty() && rootHint != postId) {
		route.query.insert(QStringLiteral("root"), rootHint);
	}
	return route.toString();
}

QString SharedPost(const QString &slug) {
	return Make(RouteType::SharedPost, slug).toString();
}

QString Topic(const QString &slug) {
	auto clean = slug;
	if (clean.startsWith(QLatin1Char('#'))) {
		clean.remove(0, 1);
	}
	return Make(RouteType::Topic, clean).toString();
}

QString EliNotifications() {
	return Make(RouteType::EliNotifications).toString();
}

QString Search(const QString &query) {
	auto route = Make(RouteType::Search);
	if (!query.isEmpty()) {
		route.query.insert(QStringLiteral("q"), query);
	}
	return route.toString();
}

QString Me() {
	return Make(RouteType::Me).toString();
}

QString Settings(const QString &section) {
	return Make(RouteType::Settings, section).toString();
}

QString ConversationTopics() {
	return Make(RouteType::ConversationTopics).toString();
}

QString Conversation(const QString &id) {
	return Make(RouteType::Conversation, id).toString();
}

QString NewGroup() {
	return Make(RouteType::NewGroup).toString();
}

QString Group(const QString &groupId) {
	return Make(RouteType::Group, groupId).toString();
}

QString GroupMedia(const QString &groupId) {
	auto result = Make(RouteType::Group, groupId);
	result.query.insert(QStringLiteral("show"), QStringLiteral("media"));
	return result.toString();
}

QString GroupAdminLog(const QString &groupId) {
	auto result = Make(RouteType::Group, groupId);
	result.query.insert(QStringLiteral("show"), QStringLiteral("log"));
	return result.toString();
}

QString GroupReactions(const QString &groupId, qint64 seq) {
	auto result = Make(RouteType::Group, groupId);
	result.query.insert(QStringLiteral("show"), QStringLiteral("reactions"));
	result.query.insert(QStringLiteral("seq"), QString::number(seq));
	return result.toString();
}

QString GroupInvite(const QString &token) {
	return Make(RouteType::GroupInvite, token).toString();
}

} // namespace Routes
} // namespace Window
