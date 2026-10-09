// Tolerant readers / writers for wire JSON. The HTTP API and the realtime
// codec agree on shapes, but a few numbers arrive as strings (64-bit ids in
// protojson, query echoes), so every reader accepts both.
#pragma once

#include "base/basic_types.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QMap>

namespace Data::Json {

[[nodiscard]] QString String(const QJsonObject &object, QLatin1StringView key);
[[nodiscard]] qint64 Int64(const QJsonValue &value, qint64 fallback = 0);
[[nodiscard]] qint64 Int64(const QJsonObject &object, QLatin1StringView key, qint64 fallback = 0);
[[nodiscard]] int Int(const QJsonObject &object, QLatin1StringView key, int fallback = 0);
[[nodiscard]] double Double(const QJsonObject &object, QLatin1StringView key, double fallback = 0.);
[[nodiscard]] bool Bool(const QJsonObject &object, QLatin1StringView key, bool fallback = false);
[[nodiscard]] bool Has(const QJsonObject &object, QLatin1StringView key); // present and not null
[[nodiscard]] QStringList Strings(const QJsonValue &value);
[[nodiscard]] QVector<qint64> Int64s(const QJsonValue &value);
[[nodiscard]] QMap<QString, qint64> CountMap(const QJsonValue &value);
[[nodiscard]] QMap<QString, QString> StringMap(const QJsonValue &value);

// A timestamp that may be seconds or milliseconds -> milliseconds.
[[nodiscard]] qint64 ToMs(qint64 value);

// Writers that leave out empty / zero / false values (like `omitempty`).
void Put(QJsonObject &object, QLatin1StringView key, const QString &value);
void Put(QJsonObject &object, QLatin1StringView key, qint64 value);
void Put(QJsonObject &object, QLatin1StringView key, int value);
void Put(QJsonObject &object, QLatin1StringView key, double value);
void Put(QJsonObject &object, QLatin1StringView key, bool value);
void Put(QJsonObject &object, QLatin1StringView key, const QJsonObject &value);
void Put(QJsonObject &object, QLatin1StringView key, const QJsonArray &value);
[[nodiscard]] QJsonArray Array(const QStringList &values);
[[nodiscard]] QJsonArray Array(const QVector<qint64> &values);
[[nodiscard]] QJsonObject Object(const QMap<QString, qint64> &values);
[[nodiscard]] QJsonObject Object(const QMap<QString, QString> &values);

template <typename Type>
[[nodiscard]] QVector<Type> List(const QJsonValue &value) {
	auto result = QVector<Type>();
	const auto list = value.toArray();
	result.reserve(list.size());
	for (const auto &item : list) {
		if (item.isObject()) {
			result.push_back(Type::FromJson(item.toObject()));
		}
	}
	return result;
}

template <typename Type>
[[nodiscard]] QJsonArray Array(const QVector<Type> &values) {
	auto result = QJsonArray();
	for (const auto &value : values) {
		result.push_back(value.toJson());
	}
	return result;
}

template <typename Type>
[[nodiscard]] std::optional<Type> Optional(const QJsonObject &object, QLatin1StringView key) {
	const auto value = object.value(key);
	return value.isObject()
		? std::make_optional(Type::FromJson(value.toObject()))
		: std::nullopt;
}

} // namespace Data::Json
