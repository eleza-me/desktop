// The API's problem+json mapped to one error type the screens can read.
#pragma once

#include "base/basic_types.h"

#include <QtCore/QJsonValue>
#include <QtCore/QMetaType>

namespace Api {

struct Error {
	int status = 0;              // HTTP status; 0 = the request never got an answer
	QString code;                // "chat.not_found", "offline", "decoding", "cancelled"
	QString message;             // localized title from the server when there is one
	bool retryable = false;
	std::optional<double> retryAfter; // seconds (Retry-After, or retry_after_s over realtime)
	QString requestId;           // X-Request-Id of the failed call (support / logs)
	QJsonValue body;             // the parsed problem document, when it was JSON

	[[nodiscard]] bool offline() const {
		return status == 0;
	}
	[[nodiscard]] bool unauthorized() const {
		return status == 401;
	}
	[[nodiscard]] bool notFound() const {
		return status == 404;
	}
	[[nodiscard]] bool rateLimited() const {
		return status == 429;
	}
	[[nodiscard]] bool serverSide() const {
		return status >= 500;
	}
	// Worth retrying later without the user doing anything (outbox policy).
	[[nodiscard]] bool transient() const {
		return status == 0 || retryable || status >= 500 || status == 429;
	}
	// "404 chat.not_found: <message> (request <id>)": everything, for tests
	// and developer tools. The message is the server's (or the network
	// stack's) text and is not for the log of a release build.
	[[nodiscard]] QString describe() const;
	// What a log line may hold: describe() in a build with the debug tools
	// (ELEZA_DEBUG_TOOLS), otherwise the status, the error code and the
	// request id only.
	[[nodiscard]] QString forLog() const;

	[[nodiscard]] static Error Offline(const QString &message = QStringLiteral("offline"));
	[[nodiscard]] static Error Cancelled();
	[[nodiscard]] static Error Local(int status, const QString &code, const QString &message, bool retryable = false);
	// From an HTTP error response.
	[[nodiscard]] static Error FromResponse(
		int status,
		const QByteArray &body,
		const QByteArray &retryAfterHeader,
		const QString &reason,
		const QString &requestId);
};

// The route of a request without anything that names a chat, a user, a post or
// a file: "/v1/chats/c1/messages/42/attachment?variant=thumb" ->
// "/v1/chats/:id/messages/:id/attachment". Only the fixed words of the API stay;
// every other segment becomes ":id", the query is dropped, an absolute URL
// (media on another host) becomes "<url>".
[[nodiscard]] QString RouteTemplate(const QString &path);
// The route as a log line may hold it: the path itself in a build with the
// debug tools, the template otherwise.
[[nodiscard]] QString RouteForLog(const QString &path);

} // namespace Api

Q_DECLARE_METATYPE(Api::Error)
