// The protobuf wire format, as much of it as the media library's messages
// need: a writer and a reader for varint, 64-bit and length-delimited fields.
//
// The library speaks proto2 with `required` fields and passes pointers as
// uint64: every field is written explicitly (also zero values) and integers
// stay exact, which is why the JSON-shaped codec of rt/ is not used here.
//
//   auto options = Calls::Ffi::Writer();
//   options.boolean(1, true);                       // auto_subscribe
//   auto request = Calls::Ffi::Writer();
//   request.string(1, url).string(2, token).message(3, options);
//
//   auto reader = Calls::Ffi::Reader(bytes);
//   while (reader.next()) {
//       switch (reader.field()) {
//       case 1: id = reader.varint(); break;
//       case 3: parse(reader.sub()); break;
//       }
//   }
//   if (!reader.ok()) { /* truncated or malformed */ }
//
// Unit-tested in tests/app/test_calls_ffi_proto.cpp.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <cstring>

namespace Calls::Ffi {

class Writer {
public:
	Writer &varint(int field, quint64 value) {
		key(field, 0);
		raw(value);
		return *this;
	}
	Writer &boolean(int field, bool value) {
		return varint(field, value ? 1 : 0);
	}
	// int32 / int64 / enum: negative values take ten bytes (two's complement).
	Writer &integer(int field, qint64 value) {
		return varint(field, quint64(value));
	}
	Writer &real(int field, double value) {
		key(field, 1);
		auto bits = quint64();
		std::memcpy(&bits, &value, sizeof(bits));
		for (auto i = 0; i != 8; ++i) {
			_data.append(char((bits >> (8 * i)) & 0xFF));
		}
		return *this;
	}
	Writer &bytes(int field, const QByteArray &value) {
		key(field, 2);
		raw(quint64(value.size()));
		_data.append(value);
		return *this;
	}
	Writer &string(int field, const QString &value) {
		return bytes(field, value.toUtf8());
	}
	Writer &message(int field, const Writer &value) {
		return bytes(field, value._data);
	}

	[[nodiscard]] const QByteArray &data() const {
		return _data;
	}

private:
	void key(int field, int wireType) {
		raw((quint64(field) << 3) | quint64(wireType));
	}
	void raw(quint64 value) {
		while (value >= 0x80) {
			_data.append(char((value & 0x7F) | 0x80));
			value >>= 7;
		}
		_data.append(char(value));
	}

	QByteArray _data;

};

// Reads one message; the bytes must outlive the reader and its sub-readers.
class Reader {
public:
	Reader() = default;
	explicit Reader(const QByteArray &bytes)
	: Reader(bytes.constData(), bytes.size()) {
	}
	Reader(const char *data, qsizetype size)
	: _at(reinterpret_cast<const uchar*>(data))
	, _end(_at + ((data && size > 0) ? size : 0)) {
	}

	// Moves to the next field; false at the end or on malformed input
	// (ok() tells which).
	[[nodiscard]] bool next() {
		if (!_ok || _at == _end) {
			return false;
		}
		const auto key = readVarint();
		if (!_ok) {
			return false;
		}
		_field = int(key >> 3);
		_wireType = int(key & 7);
		_value = 0;
		_chunk = nullptr;
		_chunkSize = 0;
		switch (_wireType) {
		case 0:
			_value = readVarint();
			break;
		case 1:
			if (_end - _at < 8) {
				_ok = false;
				break;
			}
			for (auto i = 0; i != 8; ++i) {
				_value |= quint64(_at[i]) << (8 * i);
			}
			_at += 8;
			break;
		case 2: {
			const auto size = readVarint();
			if (!_ok || size > quint64(_end - _at)) {
				_ok = false;
				break;
			}
			_chunk = _at;
			_chunkSize = qsizetype(size);
			_at += size;
		} break;
		case 5:
			if (_end - _at < 4) {
				_ok = false;
				break;
			}
			for (auto i = 0; i != 4; ++i) {
				_value |= quint64(_at[i]) << (8 * i);
			}
			_at += 4;
			break;
		default:
			_ok = false;
			break;
		}
		return _ok && (_field > 0);
	}

	[[nodiscard]] bool ok() const {
		return _ok;
	}
	[[nodiscard]] int field() const {
		return _field;
	}
	[[nodiscard]] bool isChunk() const {
		return (_wireType == 2);
	}
	[[nodiscard]] quint64 varint() const {
		return _value;
	}
	[[nodiscard]] qint64 integer() const {
		return qint64(_value);
	}
	[[nodiscard]] bool boolean() const {
		return (_value != 0);
	}
	[[nodiscard]] double real() const {
		if (_wireType != 1) {
			return 0.;
		}
		auto result = 0.;
		std::memcpy(&result, &_value, sizeof(result));
		return result;
	}
	[[nodiscard]] QByteArray bytes() const {
		return _chunk
			? QByteArray(reinterpret_cast<const char*>(_chunk), _chunkSize)
			: QByteArray();
	}
	[[nodiscard]] QString string() const {
		return _chunk
			? QString::fromUtf8(reinterpret_cast<const char*>(_chunk), _chunkSize)
			: QString();
	}
	// The nested message of the current field (empty for a scalar field).
	[[nodiscard]] Reader sub() const {
		return Reader(reinterpret_cast<const char*>(_chunk), _chunkSize);
	}

private:
	quint64 readVarint() {
		auto result = quint64();
		for (auto shift = 0; shift < 70; shift += 7) {
			if (_at == _end) {
				_ok = false;
				return 0;
			}
			const auto byte = *_at++;
			if (shift < 64) {
				result |= quint64(byte & 0x7F) << shift;
			}
			if (!(byte & 0x80)) {
				return result;
			}
		}
		_ok = false;
		return 0;
	}

	const uchar *_at = nullptr;
	const uchar *_end = nullptr;
	const uchar *_chunk = nullptr;
	qsizetype _chunkSize = 0;
	quint64 _value = 0;
	int _field = 0;
	int _wireType = 0;
	bool _ok = true;

};

} // namespace Calls::Ffi
