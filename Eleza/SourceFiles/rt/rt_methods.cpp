#include "rt/rt_methods.h"

#include "rt/rt_schema.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QUrl>

#include <cmath>

namespace Rt::Methods {
namespace {

using Schema::Field;
using Schema::Kind;

[[nodiscard]] QString ScalarString(const QJsonValue &value) {
	switch (value.type()) {
	case QJsonValue::String: return value.toString();
	case QJsonValue::Bool: return value.toBool() ? u"true"_s : u"false"_s;
	case QJsonValue::Double: {
		const auto number = value.toDouble();
		return (number == std::trunc(number) && std::abs(number) < 9e15)
			? QString::number(qint64(number))
			: QString::number(number, 'g', 17);
	}
	case QJsonValue::Array:
		return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
	case QJsonValue::Object:
		return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
	default: return QString();
	}
}

[[nodiscard]] bool IsProtoDefault(const Field &field, const QJsonValue &value) {
	if (value.isNull() || value.isUndefined()) {
		return true;
	} else if (field.optional()) {
		return false;
	} else if (field.repeated()) {
		return value.isArray() && value.toArray().isEmpty();
	}
	switch (field.kind) {
	case Kind::String:
	case Kind::Bytes: return value.isString() && value.toString().isEmpty();
	case Kind::Bool: return value.isBool() && !value.toBool();
	case Kind::Double:
	case Kind::Int64:
	case Kind::Int32:
	case Kind::Uint32:
		return (value.isDouble() && value.toDouble() == 0.)
			|| (value.isString() && value.toString().isEmpty());
	default: return false;
	}
}

} // namespace

bool Known(const QString &method) {
	const auto name = method.toLatin1();
	return Schema::FindMethod(QLatin1StringView(name)) != nullptr;
}

std::optional<HttpCall> ToHttp(const QString &method, const QJsonObject &body) {
	const auto name = method.toLatin1();
	const auto found = Schema::FindMethod(QLatin1StringView(name));
	const auto request = found ? Schema::MessageAt(found->request) : nullptr;
	if (!request) {
		return std::nullopt;
	}
	auto result = HttpCall();
	result.verb = QString::fromLatin1(found->verb);
	result.path = QString::fromLatin1(found->pattern);
	auto fields = body;
	for (auto i = 0; i != request->count; ++i) {
		const auto &field = request->fields[i];
		const auto key = QLatin1StringView(field.json);
		const auto value = fields.value(key);
		if (field.flags & Schema::kPath) {
			const auto text = ScalarString(value);
			if (text.isEmpty()) {
				return std::nullopt;
			}
			fields.remove(key);
			result.path.replace(
				u'{' + QString::fromLatin1(field.pathParam) + u'}',
				QString::fromLatin1(QUrl::toPercentEncoding(text)));
		} else if (field.flags & Schema::kQuery) {
			fields.remove(key);
			const auto text = ScalarString(value);
			if (!text.isEmpty()) {
				result.query.push_back({ QString(key), text });
			}
		} else if (!value.isUndefined() && IsProtoDefault(field, value)) {
			fields.remove(key);
		}
	}
	if (result.verb == u"GET") {
		// Leftover fields of a GET are query parameters too.
		for (auto i = fields.begin(); i != fields.end(); ++i) {
			const auto text = ScalarString(i.value());
			if (!text.isEmpty()) {
				result.query.push_back({ i.key(), text });
			}
		}
	} else if (result.verb == u"DELETE" && fields.isEmpty()) {
		// No body.
	} else {
		result.body = fields;
		result.hasBody = true;
	}
	return result;
}

} // namespace Rt::Methods
