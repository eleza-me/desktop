// A small dynamic protobuf (proto3 binary) codec over QJsonObject, driven by
// the generated schema tables (rt_schema). Values follow the JSON conventions
// of the HTTP API so the same models parse both transports:
//
//   * integers are JSON numbers (int64 is exact up to 2^53: enough for pts,
//     seq and millisecond timestamps); numeric strings are accepted on encode;
//   * bytes are base64 strings;
//   * messages are objects, repeated fields arrays, maps objects;
//   * zero / empty values are not encoded (proto3 defaults) unless the field
//     has explicit presence (`optional`);
//   * on decode the keys the HTTP API always writes (views.always.json) get
//     their zero value back, so the JSON shapes match;
//   * `<T>Row { repeated items }` wrappers are nested lists in JSON
//     ([[...]] <-> [{items: [...]}]).
//
// Envelope-level conventions (the single `body` field of a response, the
// UpdateData oneof) are handled by UnwrapBody / UnwrapData.
#pragma once

#include "rt/rt_schema.h"

#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

namespace Rt::Proto {

// nullopt: unknown message name.
[[nodiscard]] std::optional<QByteArray> Encode(
	QLatin1StringView message,
	const QJsonObject &object);
[[nodiscard]] QByteArray Encode(const Schema::Message &message, const QJsonObject &object);

// nullopt: unknown message name or malformed bytes (truncated varint /
// length, bad wire type, nesting deeper than 48).
[[nodiscard]] std::optional<QJsonObject> Decode(
	QLatin1StringView message,
	const QByteArray &bytes);
[[nodiscard]] std::optional<QJsonObject> Decode(
	const Schema::Message &message,
	const QByteArray &bytes);

// "chat.draft.save" -> { "ChatDraftSaveRequest", "ChatDraftSaveResponse" }.
struct MessageNames {
	QByteArray request;
	QByteArray response;
};
[[nodiscard]] MessageNames NamesOfMethod(const QString &method);

// A response message with the single field `body` is the HTTP body itself.
[[nodiscard]] QJsonValue UnwrapBody(const QJsonObject &object);
// The inverse for requests / tests: wraps when the message has only `body`.
[[nodiscard]] QJsonObject WrapBody(const Schema::Message &message, const QJsonValue &body);

// `data` of an update frame is { one typed field by kind }: returns that field.
[[nodiscard]] QJsonObject UnwrapData(const QJsonObject &data);

// The UpdateData field that carries an update kind ("message", "read", ...);
// empty for kinds without a payload.
[[nodiscard]] QString DataFieldOfKind(const QString &kind);

} // namespace Rt::Proto
