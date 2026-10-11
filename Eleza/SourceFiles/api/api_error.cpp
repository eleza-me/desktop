#include "api/api_error.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>

namespace Api {
namespace {

// The fixed words of the API routes. A word that is missing here is logged as
// ":id", which is the safe side.
[[nodiscard]] bool FixedSegment(QStringView segment) {
	static const auto kWords = QSet<QString>{
		u"v1"_s, u"v2"_s, u"accept"_s, u"account"_s, u"active"_s,
		u"archived"_s,
		u"attachment"_s, u"auth"_s, u"availability"_s, u"avatar"_s,
		u"birthday"_s, u"block"_s, u"blocks"_s, u"bookmark"_s,
		u"bookmarks"_s, u"by-handle"_s, u"by-slug"_s, u"callback"_s,
		u"calls"_s, u"cancel"_s,
		u"capability"_s, u"chats"_s, u"claim"_s, u"client-config"_s,
		u"complete"_s, u"confirm"_s, u"connected"_s, u"continue"_s,
		u"conversations"_s,
		u"counts"_s, u"decline"_s, u"delete-for-both"_s, u"difference"_s,
		u"disable"_s, u"dismiss"_s, u"download"_s, u"draft"_s, u"email"_s,
		u"end"_s, u"exports"_s, u"feed"_s, u"filters"_s, u"follow"_s,
		u"follow-requests"_s, u"followers"_s, u"following"_s, u"for-you"_s,
		u"handle"_s, u"handles"_s, u"hangup"_s, u"hidden-topics"_s,
		u"incoming"_s, u"installed"_s,
		u"join"_s, u"keys"_s, u"latest"_s, u"link-preview"_s,
		u"live-location"_s, u"live-locations"_s, u"logout"_s, u"me"_s,
		u"media"_s, u"member"_s, u"messages"_s, u"mute"_s,
		u"muted-words"_s, u"new"_s, u"nonce"_s, u"not-interested"_s,
		u"notifications"_s, u"open"_s, u"otp"_s, u"packs"_s,
		u"password"_s, u"people"_s, u"phone"_s, u"pin"_s,
		u"pinned-chats"_s, u"pins"_s, u"played"_s, u"qos"_s,
		u"post-notifications"_s, u"posts"_s, u"presence"_s, u"profile"_s,
		u"reaction"_s, u"reactions"_s, u"read"_s, u"reasons"_s,
		u"recover"_s, u"refresh"_s, u"reject"_s, u"rejoin"_s,
		u"relationship"_s, u"remove"_s,
		u"replies"_s, u"reply-policy"_s, u"reports"_s, u"repost"_s,
		u"request"_s, u"requests"_s, u"revoke-others"_s, u"ringing"_s,
		u"search"_s,
		u"sessions"_s, u"settings"_s, u"setup"_s, u"share"_s,
		u"share-link"_s, u"sounds"_s, u"state"_s, u"stickers"_s,
		u"stop"_s, u"suggestions"_s, u"sync"_s, u"theme"_s, u"threads"_s,
		u"token"_s, u"topics"_s, u"trending"_s, u"two-step"_s,
		u"unread"_s, u"uploads"_s, u"users"_s, u"verify"_s, u"video"_s,
		u"views"_s, u"vote"_s, u"ws"_s,
	};
	return kWords.contains(segment.toString());
}

// What follows these is typed by a person (a handle, a slug): never kept,
// whatever it spells.
[[nodiscard]] bool NamesFollow(QStringView segment) {
	return (segment == u"by-handle")
		|| (segment == u"by-slug")
		|| (segment == u"handles");
}

} // namespace

QString RouteTemplate(const QString &path) {
	if (!path.startsWith(u'/')) {
		return path.isEmpty() ? QString() : u"<url>"_s;
	}
	auto end = path.size();
	for (const auto mark : { u'?', u'#' }) {
		const auto index = path.indexOf(mark);
		if (index >= 0 && index < end) {
			end = index;
		}
	}
	auto result = QString();
	auto named = false;
	const auto segments = QStringView(path).left(end).split(u'/', Qt::SkipEmptyParts);
	for (const auto &segment : segments) {
		result += u'/';
		if (!named && FixedSegment(segment)) {
			result += segment;
			named = NamesFollow(segment);
		} else {
			result += u":id"_s;
			named = false;
		}
	}
	return result.isEmpty() ? u"/"_s : result;
}

QString RouteForLog(const QString &path) {
#ifdef ELEZA_DEBUG_TOOLS
	return path;
#else // ELEZA_DEBUG_TOOLS
	return RouteTemplate(path);
#endif // ELEZA_DEBUG_TOOLS
}

QString Error::forLog() const {
#ifdef ELEZA_DEBUG_TOOLS
	return describe();
#else // ELEZA_DEBUG_TOOLS
	auto result = QString::number(status);
	if (!code.isEmpty()) {
		result += u' ' + code;
	}
	if (!requestId.isEmpty()) {
		result += u" (request "_s + requestId + u')';
	}
	return result;
#endif // ELEZA_DEBUG_TOOLS
}

QString Error::describe() const {
	auto result = QString::number(status);
	if (!code.isEmpty()) {
		result += u' ' + code;
	}
	if (!message.isEmpty() && message != code) {
		result += u": "_s + message;
	}
	if (!requestId.isEmpty()) {
		result += u" (request "_s + requestId + u')';
	}
	return result;
}

Error Error::Offline(const QString &message) {
	auto result = Error();
	result.status = 0;
	result.code = u"offline"_s;
	result.message = message;
	result.retryable = true;
	return result;
}

Error Error::Cancelled() {
	auto result = Error();
	result.status = 0;
	result.code = u"cancelled"_s;
	result.message = u"cancelled"_s;
	result.retryable = false;
	return result;
}

Error Error::Local(int status, const QString &code, const QString &message, bool retryable) {
	auto result = Error();
	result.status = status;
	result.code = code;
	result.message = message;
	result.retryable = retryable;
	return result;
}

Error Error::FromResponse(
		int status,
		const QByteArray &body,
		const QByteArray &retryAfterHeader,
		const QString &reason,
		const QString &requestId) {
	auto result = Error();
	result.status = status;
	result.requestId = requestId;
	auto parseError = QJsonParseError();
	const auto document = body.isEmpty()
		? QJsonDocument()
		: QJsonDocument::fromJson(body, &parseError);
	const auto json = document.object();
	if (document.isObject()) {
		result.body = json;
	} else if (document.isArray()) {
		result.body = document.array();
	}
	const auto nested = json.value(u"error"_s).toObject();
	result.code = json.value(u"code"_s).isString()
		? json.value(u"code"_s).toString()
		: nested.value(u"code"_s).toString();
	result.message = json.value(u"title"_s).isString()
		? json.value(u"title"_s).toString()
		: json.value(u"message"_s).isString()
		? json.value(u"message"_s).toString()
		: nested.value(u"message"_s).isString()
		? nested.value(u"message"_s).toString()
		: reason;
	result.retryable = json.value(u"retryable"_s).isBool()
		? json.value(u"retryable"_s).toBool()
		: (status >= 500 || status == 429);
	if (!retryAfterHeader.isEmpty()) {
		auto ok = false;
		const auto seconds = retryAfterHeader.trimmed().toDouble(&ok);
		if (ok && seconds > 0.) {
			result.retryAfter = seconds;
		}
	}
	return result;
}

} // namespace Api
