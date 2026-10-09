// The realtime v2 protobuf schema as static tables (generated into
// rt_schema_auto.cpp by codegen/scripts/gen_rtschema.py from
// Resources/proto/*.proto). The dynamic codec (rt_proto) and the RPC -> HTTP
// bridge (rt_methods) are driven by these tables; there are no generated
// message classes.
#pragma once

#include "base/basic_types.h"

namespace Rt::Schema {

enum class Kind : uchar {
	Double,  // double, float
	Int64,   // int64, uint64, sint64, fixed64, sfixed64 (varint on our wire)
	Int32,   // int32, sint32, sfixed32
	Uint32,  // uint32, fixed32
	Bool,
	String,
	Bytes,   // base64 string in JSON
	Message, // `message` is the index of the nested message
	Map,     // `mapKey` / `mapValue` (+ `message` when the value is a message)
};

enum FieldFlag : uchar {
	kRepeated = 0x01,
	kOptional = 0x02, // explicit presence: zero values are written
	kAlways = 0x04,   // the HTTP API always writes the key: restored on decode
	kQuery = 0x08,    // request field that is a query parameter of the HTTP route
	kPath = 0x10,     // request field that is a path parameter of the HTTP route
};

struct Field {
	quint32 number = 0;
	const char *json = nullptr;      // JSON key (json_name, or the field name)
	Kind kind = Kind::String;
	Kind mapKey = Kind::String;
	Kind mapValue = Kind::String;
	qint16 message = -1;
	uchar flags = 0;
	const char *pathParam = nullptr; // "{id}" -> "id" for kPath fields

	[[nodiscard]] bool repeated() const {
		return (flags & kRepeated) != 0;
	}
	[[nodiscard]] bool optional() const {
		return (flags & kOptional) != 0;
	}
	[[nodiscard]] bool always() const {
		return (flags & kAlways) != 0;
	}
};

struct Message {
	const char *name = nullptr;
	const Field *fields = nullptr;
	int count = 0;
	// A wire wrapper of a nested list: exactly one repeated field "items".
	// JSON carries the inner list itself ([[...]] <-> [{items: [...]}]).
	bool row = false;
};

// One RPC method and the HTTP route it is bridged to (backend rtspec).
struct Method {
	const char *name = nullptr;    // "chat.send"
	const char *verb = nullptr;    // "POST"
	const char *pattern = nullptr; // "/v1/chats/{id}/messages"
	qint16 request = -1;           // message index of <Method>Request
	qint16 response = -1;          // message index of <Method>Response
};

// Messages sorted by name.
[[nodiscard]] const Message *Messages();
[[nodiscard]] int MessagesCount();
[[nodiscard]] const Method *Methods();
[[nodiscard]] int MethodsCount();

[[nodiscard]] const Message *FindMessage(QLatin1StringView name);
[[nodiscard]] const Message *MessageAt(int index);
[[nodiscard]] const Method *FindMethod(QLatin1StringView name);

} // namespace Rt::Schema
