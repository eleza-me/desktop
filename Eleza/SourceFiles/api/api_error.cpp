#include "api/api_error.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace Api {

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
