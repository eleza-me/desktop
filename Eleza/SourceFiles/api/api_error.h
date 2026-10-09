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
	// "404 chat.not_found: ..." for logs.
	[[nodiscard]] QString describe() const;

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

} // namespace Api

Q_DECLARE_METATYPE(Api::Error)
