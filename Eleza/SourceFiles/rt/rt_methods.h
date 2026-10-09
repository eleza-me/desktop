// The realtime RPC surface and its HTTP twin. Every method the gateway
// accepts is bridged by the server to one HTTP route (backend rtspec); the
// request message of a method = path parameters + query parameters + the
// route's JSON body. ToHttp performs the same mapping on the client, so a
// call that cannot go over the connection is sent as the equivalent HTTP
// request (same Idempotency-Key, hence safe to repeat).
#pragma once

#include "base/basic_types.h"

#include <QtCore/QJsonObject>
#include <QtCore/QPair>

namespace Rt::Methods {

struct HttpCall {
	QString verb;                            // "GET", "POST", ...
	QString path;                            // "/v1/chats/c1/messages"
	QVector<QPair<QString, QString>> query;  // non-empty parameters, in schema order
	QJsonObject body;                        // JSON body when hasBody
	bool hasBody = false;
};

// True for the methods of the generated table ("chat.send", ...).
[[nodiscard]] bool Known(const QString &method);

// nullopt: unknown method or a missing path parameter.
// Like the wire encoding, top-level fields holding a proto3 default (empty
// string, 0, false) are left out unless the field has explicit presence.
[[nodiscard]] std::optional<HttpCall> ToHttp(const QString &method, const QJsonObject &body);

} // namespace Rt::Methods
