#include "rt/rt_proto.h"

#include "base/bytes.h"

#include <QtCore/QJsonArray>

#include <cmath>
#include <cstring>

namespace Rt::Proto {
namespace {

using Schema::Field;
using Schema::Kind;
using Schema::Message;

constexpr auto kMaxDepth = 48;
// Field numbers are 1 .. 2^29 - 1 (anything else is not protobuf).
constexpr auto kMaxFieldNumber = quint64(0x1FFFFFFF);
// JSON values one top-level Decode() may produce. An envelope is at most
// 256 KiB, but a two-byte empty sub-message expands to an object with every
// "always" key of its type: without a budget a small frame of a hostile peer
// becomes hundreds of megabytes of JSON.
constexpr auto kMaxValues = 400000;

enum Wire : int {
	kVarint = 0,
	kFixed64 = 1,
	kLengthDelimited = 2,
	kFixed32 = 5,
};

class Writer {
public:
	void byte(uchar value) {
		_data.append(char(value));
	}
	void varint(quint64 value) {
		while (value >= 0x80) {
			byte(uchar(value & 0x7F) | 0x80);
			value >>= 7;
		}
		byte(uchar(value));
	}
	void tag(quint32 field, int wire) {
		varint((quint64(field) << 3) | quint64(wire));
	}
	void bytes(const QByteArray &value) {
		varint(quint64(value.size()));
		_data.append(value);
	}
	void fixedDouble(double value) {
		auto bits = quint64(0);
		std::memcpy(&bits, &value, sizeof(bits));
		for (auto i = 0; i != 8; ++i) {
			byte(uchar(bits & 0xFF));
			bits >>= 8;
		}
	}
	[[nodiscard]] QByteArray finish() {
		if (_data.isNull()) {
			return QByteArray("", 0);
		}
		return std::move(_data);
	}

private:
	QByteArray _data;

};

class Reader {
public:
	explicit Reader(const QByteArray &data)
	: _data(reinterpret_cast<const uchar*>(data.constData()))
	, _size(data.size()) {
	}

	[[nodiscard]] bool atEnd() const {
		return _position >= _size;
	}
	[[nodiscard]] bool failed() const {
		return _failed;
	}
	[[nodiscard]] quint64 varint() {
		auto result = quint64(0);
		auto shift = 0;
		for (;;) {
			if (_position >= _size || shift > 63) {
				_failed = true;
				return 0;
			}
			const auto value = _data[_position++];
			result |= quint64(value & 0x7F) << shift;
			if (!(value & 0x80)) {
				return result;
			}
			shift += 7;
		}
	}
	[[nodiscard]] QByteArray bytes() {
		const auto length = varint();
		if (_failed || length > quint64(_size - _position)) {
			_failed = true;
			return QByteArray();
		}
		const auto result = QByteArray(
			reinterpret_cast<const char*>(_data + _position),
			qsizetype(length));
		_position += qsizetype(length);
		return result;
	}
	[[nodiscard]] double fixedDouble() {
		if (_size - _position < 8) {
			_failed = true;
			return 0.;
		}
		auto bits = quint64(0);
		for (auto i = 0; i != 8; ++i) {
			bits |= quint64(_data[_position + i]) << (8 * i);
		}
		_position += 8;
		auto result = 0.;
		std::memcpy(&result, &bits, sizeof(result));
		return result;
	}
	void skip(int wire) {
		switch (wire) {
		case kVarint: (void)varint(); break;
		case kFixed64: advance(8); break;
		case kLengthDelimited: (void)bytes(); break;
		case kFixed32: advance(4); break;
		default: _failed = true; break;
		}
	}

private:
	void advance(qsizetype count) {
		if (_size - _position < count) {
			_failed = true;
		} else {
			_position += count;
		}
	}

	const uchar *_data = nullptr;
	qsizetype _size = 0;
	qsizetype _position = 0;
	bool _failed = false;

};

[[nodiscard]] QLatin1StringView Key(const Field &field) {
	return QLatin1StringView(field.json);
}

[[nodiscard]] double ToDouble(const QJsonValue &value) {
	switch (value.type()) {
	case QJsonValue::Double: return value.toDouble();
	case QJsonValue::Bool: return value.toBool() ? 1. : 0.;
	case QJsonValue::String: {
		auto ok = false;
		const auto result = value.toString().toDouble(&ok);
		return (ok && std::isfinite(result)) ? result : 0.;
	}
	default: return 0.;
	}
}

[[nodiscard]] qint64 ToInt64(const QJsonValue &value) {
	if (value.isString()) {
		// Exact for the whole int64 / uint64 range (protojson writes them as strings).
		const auto text = value.toString();
		auto ok = false;
		const auto parsed = text.toLongLong(&ok);
		if (ok) {
			return parsed;
		}
		const auto unsignedParsed = text.toULongLong(&ok);
		if (ok) {
			return qint64(unsignedParsed);
		}
	}
	const auto number = std::trunc(ToDouble(value));
	if (!(number == number)) {
		return 0;
	} else if (number >= 9223372036854775807.) {
		return std::numeric_limits<qint64>::max();
	} else if (number <= -9223372036854775808.) {
		return std::numeric_limits<qint64>::min();
	}
	return qint64(number);
}

[[nodiscard]] bool ToBool(const QJsonValue &value) {
	return value.isBool() ? value.toBool() : (ToDouble(value) != 0.);
}

[[nodiscard]] QByteArray ToBytes(const QJsonValue &value) {
	return value.isString() ? base::bytes::fromBase64(value.toString()) : QByteArray();
}

[[nodiscard]] QString ToString(const QJsonValue &value) {
	switch (value.type()) {
	case QJsonValue::String: return value.toString();
	case QJsonValue::Double: {
		const auto number = value.toDouble();
		return (number == std::trunc(number) && std::abs(number) < 9e15)
			? QString::number(base::SafeInt64(number))
			: QString::number(number, 'g', 17);
	}
	case QJsonValue::Bool: return value.toBool() ? u"true"_s : u"false"_s;
	default: return QString();
	}
}

void EncodeNumeric(Writer &to, Kind kind, const QJsonValue &value) {
	switch (kind) {
	case Kind::Double: to.fixedDouble(ToDouble(value)); break;
	case Kind::Bool: to.byte(ToBool(value) ? 1 : 0); break;
	// Negative int32 is sign-extended to ten bytes, as protobuf defines.
	case Kind::Int32: to.varint(quint64(qint64(qint32(ToInt64(value))))); break;
	case Kind::Uint32: to.varint(quint64(quint32(ToInt64(value)))); break;
	default: to.varint(quint64(ToInt64(value))); break;
	}
}

void EncodeScalar(Writer &to, Kind kind, quint32 number, const QJsonValue &value, bool force) {
	switch (kind) {
	case Kind::String: {
		const auto text = ToString(value);
		if (text.isEmpty() && !force) {
			return;
		}
		to.tag(number, kLengthDelimited);
		to.bytes(text.toUtf8());
	} break;
	case Kind::Bytes: {
		const auto bytes = ToBytes(value);
		if (bytes.isEmpty() && !force) {
			return;
		}
		to.tag(number, kLengthDelimited);
		to.bytes(bytes);
	} break;
	case Kind::Bool: {
		const auto flag = ToBool(value);
		if (!flag && !force) {
			return;
		}
		to.tag(number, kVarint);
		to.byte(flag ? 1 : 0);
	} break;
	case Kind::Double: {
		const auto number64 = ToDouble(value);
		if (number64 == 0. && !force) {
			return;
		}
		to.tag(number, kFixed64);
		to.fixedDouble(number64);
	} break;
	case Kind::Int64:
	case Kind::Int32:
	case Kind::Uint32: {
		if (ToInt64(value) == 0 && !force) {
			return;
		}
		to.tag(number, kVarint);
		EncodeNumeric(to, kind, value);
	} break;
	case Kind::Message:
	case Kind::Map:
		break;
	}
}

void EncodeMessage(Writer &to, const Message &message, const QJsonObject &object);

// A row message accepts the inner list itself.
[[nodiscard]] QJsonObject RowObject(const Message &message, const QJsonValue &item) {
	if (message.row && item.isArray()) {
		return QJsonObject{ { Key(message.fields[0]), item } };
	}
	return item.toObject();
}

void EncodeField(Writer &to, const Field &field, const QJsonValue &value) {
	if (field.kind == Kind::Map) {
		if (!value.isObject()) {
			return;
		}
		const auto object = value.toObject();
		const auto nested = Schema::MessageAt(field.message);
		for (auto i = object.begin(); i != object.end(); ++i) {
			auto entry = Writer();
			EncodeScalar(entry, field.mapKey, 1, QJsonValue(i.key()), true);
			if (field.mapValue == Kind::Message) {
				if (!nested || !i.value().isObject()) {
					continue;
				}
				auto sub = Writer();
				EncodeMessage(sub, *nested, i.value().toObject());
				entry.tag(2, kLengthDelimited);
				entry.bytes(sub.finish());
			} else {
				EncodeScalar(entry, field.mapValue, 2, i.value(), true);
			}
			to.tag(field.number, kLengthDelimited);
			to.bytes(entry.finish());
		}
		return;
	}
	if (field.repeated()) {
		if (!value.isArray()) {
			return;
		}
		const auto list = value.toArray();
		if (list.isEmpty()) {
			return;
		}
		if (field.kind == Kind::Message) {
			const auto nested = Schema::MessageAt(field.message);
			if (!nested) {
				return;
			}
			for (const auto &item : list) {
				if (!item.isObject() && !(nested->row && item.isArray())) {
					continue;
				}
				auto sub = Writer();
				EncodeMessage(sub, *nested, RowObject(*nested, item));
				to.tag(field.number, kLengthDelimited);
				to.bytes(sub.finish());
			}
		} else if (field.kind == Kind::String || field.kind == Kind::Bytes) {
			for (const auto &item : list) {
				EncodeScalar(to, field.kind, field.number, item, true);
			}
		} else {
			auto packed = Writer();
			for (const auto &item : list) {
				EncodeNumeric(packed, field.kind, item);
			}
			to.tag(field.number, kLengthDelimited);
			to.bytes(packed.finish());
		}
		return;
	}
	if (field.kind == Kind::Message) {
		const auto nested = Schema::MessageAt(field.message);
		if (!nested || !value.isObject()) {
			return;
		}
		auto sub = Writer();
		EncodeMessage(sub, *nested, value.toObject());
		to.tag(field.number, kLengthDelimited);
		to.bytes(sub.finish());
		return;
	}
	EncodeScalar(to, field.kind, field.number, value, field.optional());
}

void EncodeMessage(Writer &to, const Message &message, const QJsonObject &object) {
	for (auto i = 0; i != message.count; ++i) {
		const auto &field = message.fields[i];
		const auto value = object.value(Key(field));
		if (value.isUndefined() || value.isNull()) {
			continue;
		}
		EncodeField(to, field, value);
	}
}

[[nodiscard]] QJsonValue DefaultValue(Kind kind) {
	switch (kind) {
	case Kind::String:
	case Kind::Bytes: return QJsonValue(QString());
	case Kind::Bool: return QJsonValue(false);
	default: return QJsonValue(0);
	}
}

// Reads one scalar; a mismatching wire type is skipped and yields the default.
[[nodiscard]] QJsonValue DecodeScalar(Kind kind, int wire, Reader &from) {
	switch (kind) {
	case Kind::String:
		if (wire != kLengthDelimited) {
			from.skip(wire);
			return QJsonValue(QString());
		}
		return QJsonValue(QString::fromUtf8(from.bytes()));
	case Kind::Bytes:
		if (wire != kLengthDelimited) {
			from.skip(wire);
			return QJsonValue(QString());
		}
		return QJsonValue(base::bytes::toBase64(from.bytes()));
	case Kind::Bool:
		if (wire != kVarint) {
			from.skip(wire);
			return QJsonValue(false);
		}
		return QJsonValue(from.varint() != 0);
	case Kind::Double:
		if (wire != kFixed64) {
			from.skip(wire);
			return QJsonValue(0);
		}
		return QJsonValue(from.fixedDouble());
	case Kind::Int32:
		if (wire != kVarint) {
			from.skip(wire);
			return QJsonValue(0);
		}
		return QJsonValue(double(qint32(quint32(from.varint()))));
	case Kind::Uint32:
		if (wire != kVarint) {
			from.skip(wire);
			return QJsonValue(0);
		}
		return QJsonValue(double(quint32(from.varint())));
	default:
		if (wire != kVarint) {
			from.skip(wire);
			return QJsonValue(0);
		}
		return QJsonValue(double(qint64(from.varint())));
	}
}

[[nodiscard]] bool DecodeMessage(
	const Message &message,
	const QByteArray &bytes,
	int depth,
	int &budget,
	QJsonObject &result);

[[nodiscard]] const Field *FindField(const Message &message, quint32 number) {
	for (auto i = 0; i != message.count; ++i) {
		if (message.fields[i].number == number) {
			return &message.fields[i];
		}
	}
	return nullptr;
}

// A row message is represented by its inner list.
[[nodiscard]] QJsonValue RowValue(const Message &message, const QJsonObject &object) {
	if (message.row) {
		return QJsonValue(object.value(Key(message.fields[0])).toArray());
	}
	return QJsonValue(object);
}

// Repeated fields and maps are taken out of `result`, extended and put back:
// appending to a copy that `result` still shares would copy the whole
// container for every element (quadratic in the number of elements).
[[nodiscard]] bool DecodeField(
		const Field &field,
		int wire,
		Reader &from,
		int depth,
		int &budget,
		QJsonObject &result) {
	if (--budget < 0) {
		return false;
	}
	const auto key = Key(field);
	if (field.kind == Kind::Map) {
		if (wire != kLengthDelimited) {
			from.skip(wire);
			return !from.failed();
		}
		const auto entryBytes = from.bytes();
		if (from.failed()) {
			return false;
		}
		const auto nested = Schema::MessageAt(field.message);
		auto entry = Reader(entryBytes);
		auto entryKey = QString();
		auto entryValue = QJsonValue(QJsonValue::Undefined);
		while (!entry.atEnd()) {
			const auto tag = entry.varint();
			if (entry.failed()) {
				return false;
			}
			const auto number = quint32(qMin(tag >> 3, kMaxFieldNumber + 1));
			const auto entryWire = int(tag & 7);
			if (number == 1) {
				entryKey = ToString(DecodeScalar(field.mapKey, entryWire, entry));
			} else if (number == 2) {
				if (field.mapValue == Kind::Message) {
					if (entryWire != kLengthDelimited || !nested) {
						entry.skip(entryWire);
					} else {
						auto sub = QJsonObject();
						const auto subBytes = entry.bytes();
						if (entry.failed()
							|| !DecodeMessage(*nested, subBytes, depth + 1, budget, sub)) {
							return false;
						}
						entryValue = sub;
					}
				} else {
					entryValue = DecodeScalar(field.mapValue, entryWire, entry);
				}
			} else {
				entry.skip(entryWire);
			}
			if (entry.failed()) {
				return false;
			}
		}
		if (entryValue.isUndefined()) {
			entryValue = (field.mapValue == Kind::Message)
				? QJsonValue(QJsonObject())
				: DefaultValue(field.mapValue);
		}
		auto map = result.take(key).toObject();
		map.insert(entryKey, entryValue);
		result.insert(key, map);
		return true;
	}
	if (field.kind == Kind::Message) {
		const auto nested = Schema::MessageAt(field.message);
		if (wire != kLengthDelimited || !nested) {
			from.skip(wire);
			return !from.failed();
		}
		const auto subBytes = from.bytes();
		auto sub = QJsonObject();
		if (from.failed() || !DecodeMessage(*nested, subBytes, depth + 1, budget, sub)) {
			return false;
		}
		if (field.repeated()) {
			auto list = result.take(key).toArray();
			list.append(RowValue(*nested, sub));
			result.insert(key, list);
		} else {
			result.insert(key, sub);
		}
		return true;
	}
	if (field.repeated()) {
		auto list = result.take(key).toArray();
		if (field.kind == Kind::String || field.kind == Kind::Bytes) {
			list.append(DecodeScalar(field.kind, wire, from));
		} else if (wire == kLengthDelimited) {
			const auto packedBytes = from.bytes();
			if (from.failed()) {
				return false;
			}
			auto packed = Reader(packedBytes);
			const auto itemWire = (field.kind == Kind::Double) ? kFixed64 : kVarint;
			while (!packed.atEnd()) {
				list.append(DecodeScalar(field.kind, itemWire, packed));
				if (packed.failed() || --budget < 0) {
					return false;
				}
			}
		} else {
			list.append(DecodeScalar(field.kind, wire, from));
		}
		result.insert(key, list);
		return !from.failed();
	}
	result.insert(key, DecodeScalar(field.kind, wire, from));
	return !from.failed();
}

bool DecodeMessage(
		const Message &message,
		const QByteArray &bytes,
		int depth,
		int &budget,
		QJsonObject &result) {
	if (depth > kMaxDepth) {
		return false;
	}
	auto from = Reader(bytes);
	while (!from.atEnd()) {
		const auto tag = from.varint();
		if (from.failed()) {
			return false;
		}
		if ((tag >> 3) == 0 || (tag >> 3) > kMaxFieldNumber) {
			return false;
		}
		const auto number = quint32(tag >> 3);
		const auto wire = int(tag & 7);
		if (const auto field = FindField(message, number)) {
			if (!DecodeField(*field, wire, from, depth, budget, result)) {
				return false;
			}
		} else {
			from.skip(wire);
			if (from.failed()) {
				return false;
			}
		}
	}
	for (auto i = 0; i != message.count; ++i) {
		const auto &field = message.fields[i];
		const auto key = Key(field);
		if (!field.always() || result.contains(key)) {
			continue;
		}
		if (--budget < 0) {
			return false;
		}
		if (field.repeated()) {
			result.insert(key, QJsonArray());
		} else if (field.kind == Kind::Message) {
			auto sub = QJsonObject();
			const auto nested = Schema::MessageAt(field.message);
			if (nested && !DecodeMessage(*nested, QByteArray(), depth + 1, budget, sub)) {
				return false;
			}
			result.insert(key, sub);
		} else if (field.kind == Kind::Map) {
			result.insert(key, QJsonObject());
		} else {
			result.insert(key, DefaultValue(field.kind));
		}
	}
	return true;
}

} // namespace

std::optional<QByteArray> Encode(QLatin1StringView message, const QJsonObject &object) {
	const auto found = Schema::FindMessage(message);
	if (!found) {
		return std::nullopt;
	}
	return Encode(*found, object);
}

QByteArray Encode(const Schema::Message &message, const QJsonObject &object) {
	auto to = Writer();
	EncodeMessage(to, message, object);
	return to.finish();
}

std::optional<QJsonObject> Decode(QLatin1StringView message, const QByteArray &bytes) {
	const auto found = Schema::FindMessage(message);
	if (!found) {
		return std::nullopt;
	}
	return Decode(*found, bytes);
}

std::optional<QJsonObject> Decode(const Schema::Message &message, const QByteArray &bytes) {
	auto result = QJsonObject();
	auto budget = int(kMaxValues);
	if (!DecodeMessage(message, bytes, 0, budget, result)) {
		return std::nullopt;
	}
	return result;
}

MessageNames NamesOfMethod(const QString &method) {
	auto base = QByteArray();
	auto upper = true;
	for (const auto ch : method) {
		if (ch == u'.' || ch == u'_') {
			upper = true;
		} else if (upper) {
			base.append(ch.toUpper().toLatin1());
			upper = false;
		} else {
			base.append(ch.toLatin1());
		}
	}
	return { base + "Request", base + "Response" };
}

QJsonValue UnwrapBody(const QJsonObject &object) {
	if (object.size() == 1) {
		const auto body = object.value(QLatin1StringView("body"));
		if (body.isObject() || body.isArray()) {
			return body;
		}
	}
	return object;
}

QJsonObject WrapBody(const Schema::Message &message, const QJsonValue &body) {
	if (message.count == 1 && QLatin1StringView(message.fields[0].json) == QLatin1StringView("body")) {
		const auto object = body.toObject();
		if (!(object.size() == 1 && object.contains(QLatin1StringView("body")))) {
			return QJsonObject{ { u"body"_s, body } };
		}
	}
	return body.toObject();
}

QJsonObject UnwrapData(const QJsonObject &data) {
	if (data.size() == 1) {
		const auto inner = data.begin().value();
		if (inner.isObject()) {
			return inner.toObject();
		}
	}
	return data;
}

QString DataFieldOfKind(const QString &kind) {
	if (kind == u"message" || kind == u"edited" || kind == u"deleted" || kind == u"live_stopped") {
		return u"message"_s;
	} else if (kind == u"read" || kind == u"reaction" || kind == u"played"
		|| kind == u"cleared" || kind == u"member" || kind == u"state"
		|| kind == u"draft" || kind == u"notif" || kind == u"group") {
		return kind;
	} else if (kind == u"message_pinned" || kind == u"message_unpinned") {
		return u"pin"_s;
	}
	return QString();
}

} // namespace Rt::Proto
