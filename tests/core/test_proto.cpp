// The dynamic protobuf codec: the web client's proto.test.ts ported, plus the conventions
// the desktop codec adds (row wrappers, body / data unwrapping, the method table).
#include "base/bytes.h"
#include "rt/rt_methods.h"
#include "rt/rt_proto.h"
#include "rt/rt_schema_auto.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtTest/QtTest>

using namespace Rt;

namespace {

[[nodiscard]] QJsonObject Json(const char *text) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(QByteArray(text), &error);
	Q_ASSERT_X(error.error == QJsonParseError::NoError, "Json", text);
	return document.object();
}

[[nodiscard]] QJsonObject RoundTrip(const char *message, const QJsonObject &object) {
	const auto bytes = Proto::Encode(QLatin1StringView(message), object);
	Q_ASSERT(bytes);
	const auto back = Proto::Decode(QLatin1StringView(message), *bytes);
	Q_ASSERT(back);
	return *back;
}

} // namespace

class TestProto final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void schemaTables();
	void envelopeWithRequest();
	void knownBytes();
	void mapsRepeatedOptionalNegative();
	void alwaysFields();
	void methodNames();
	void rowsAreNestedLists();
	void numericStringsAndLargeIntegers();
	void unknownFieldsAndMalformedInput();
	void unwrapConventions();
	void updateFrameShapes();
	void httpBridge();
	void hostileBytes();
	void repeatedFieldsAreLinearAndBudgeted();

};

void TestProto::schemaTables() {
	QCOMPARE(Schema::MessagesCount(), Schema::kGeneratedMessages);
	QCOMPARE(Schema::MethodsCount(), Schema::kGeneratedMethods);
	// Sorted (the lookups are binary searches).
	for (auto i = 1; i < Schema::MessagesCount(); ++i) {
		QVERIFY(QLatin1StringView(Schema::Messages()[i - 1].name) < QLatin1StringView(Schema::Messages()[i].name));
	}
	for (auto i = 1; i < Schema::MethodsCount(); ++i) {
		QVERIFY(QLatin1StringView(Schema::Methods()[i - 1].name) < QLatin1StringView(Schema::Methods()[i].name));
	}
	QVERIFY(Schema::FindMessage("Envelope"_L1));
	QVERIFY(Schema::FindMessage("MessageView"_L1));
	QVERIFY(!Schema::FindMessage("Nope"_L1));
	QVERIFY(!Proto::Encode("Nope"_L1, QJsonObject()).has_value());
	QVERIFY(!Proto::Decode("Nope"_L1, QByteArray()).has_value());
}

void TestProto::envelopeWithRequest() {
	const auto body = Proto::Encode("Auth"_L1, Json(R"({"token":"tok","background":false,"pts":12345678901,"device":"dev","app":"desktop/0.1.0","lang":"fa"})"));
	QVERIFY(body.has_value());
	const auto envelope = Proto::Encode("Envelope"_L1, QJsonObject{
		{ u"req"_s, QJsonObject{
			{ u"id"_s, 1 },
			{ u"method"_s, u"auth"_s },
			{ u"body"_s, base::bytes::toBase64(*body) } } } });
	QVERIFY(envelope.has_value());
	const auto back = Proto::Decode("Envelope"_L1, *envelope);
	QVERIFY(back.has_value());
	const auto request = back->value("req"_L1).toObject();
	QCOMPARE(request["id"].toInt(), 1);
	QCOMPARE(request["method"].toString(), u"auth"_s);
	const auto auth = Proto::Decode("Auth"_L1, base::bytes::fromBase64(request["body"].toString()));
	QVERIFY(auth.has_value());
	QCOMPARE(auth->value("token"_L1).toString(), u"tok"_s);
	QCOMPARE(qint64(auth->value("pts"_L1).toDouble()), qint64(12345678901));
	QCOMPARE(auth->value("device"_L1).toString(), u"dev"_s);
	QCOMPARE(auth->value("app"_L1).toString(), u"desktop/0.1.0"_s);
	QCOMPARE(auth->value("lang"_L1).toString(), u"fa"_s);
	QVERIFY(!auth->contains("background"_L1)); // proto3 zero left off the wire
}

void TestProto::knownBytes() {
	// field 1 varint 150 -> 08 96 01 (the protobuf documentation example)
	QCOMPARE(Proto::Encode("Ack"_L1, Json(R"({"pts":150})")).value(), QByteArray::fromHex("089601"));
	QCOMPARE(
		Proto::Encode("Challenge"_L1, QJsonObject{ { u"nonce"_s, base::bytes::toBase64(QByteArray::fromHex("010203")) } }).value(),
		QByteArray::fromHex("0a03010203"));
	// string field 2 "testing" -> 12 07 74 65 73 74 69 6e 67
	QCOMPARE(Proto::Encode("Request"_L1, Json(R"({"method":"testing"})")).value(), QByteArray::fromHex("120774657374696e67"));
	// double 1.0 in field 1 (fixed64, little endian)
	QCOMPARE(Proto::Encode("Location"_L1, Json(R"({"lat":1})")).value(), QByteArray::fromHex("09000000000000f03f"));
	// negative int32 is ten bytes
	QCOMPARE(Proto::Encode("MediaMeta"_L1, Json(R"({"w":-1})")).value(), QByteArray::fromHex("08ffffffffffffffffff01"));
	// empty object -> empty, non-null bytes
	QCOMPARE(Proto::Encode("Ack"_L1, QJsonObject()).value().size(), 0);
}

void TestProto::mapsRepeatedOptionalNegative() {
	const auto view = Json(R"({
		"chat":"c1","seq":7,"id":"m1","sender":"u1","kind":"text","text":"سلام","created_at":1700000000000,
		"reactions":{"❤️":2,"👍":1},
		"entities":[{"type":"spoiler","offset":0,"length":2}],
		"media":{"w":100,"h":50,"duration_ms":0,"waveform":"CQg="}})");
	const auto back = RoundTrip("MessageView", view);
	QCOMPARE(back["reactions"].toObject(), Json(R"({"❤️":2,"👍":1})"));
	// `offset` is an always-key: restored although zero is not on the wire.
	QCOMPARE(back["entities"].toArray(), QJsonArray{ Json(R"({"type":"spoiler","offset":0,"length":2})") });
	QCOMPARE(back["media"].toObject()["w"].toInt(), 100);
	QCOMPARE(back["media"].toObject()["waveform"].toString(), u"CQg="_s);
	QVERIFY(!back["media"].toObject().contains(u"duration_ms"_s));
	QCOMPARE(qint64(back["created_at"].toDouble()), qint64(1700000000000));
	QCOMPARE(back["text"].toString(), u"سلام"_s);

	// explicit presence: optional bool false is written and read back
	const auto member = RoundTrip("ChatMemberRequest", Json(R"({"chat":"c","muted":false,"archived":true})"));
	QVERIFY(member.contains(u"muted"_s));
	QCOMPARE(member["muted"].toBool(), false);
	QCOMPARE(member["archived"].toBool(), true);
	const auto absent = RoundTrip("ChatMemberRequest", Json(R"({"chat":"c"})"));
	QVERIFY(!absent.contains(u"muted"_s));

	// int32 negative
	QCOMPARE(RoundTrip("MediaMeta", Json(R"({"w":-5})"))["w"].toInt(), -5);
	// int64 negative
	QCOMPARE(qint64(RoundTrip("Ack", Json(R"({"pts":-42})"))["pts"].toDouble()), qint64(-42));
	// repeated strings and packed numbers
	QCOMPARE(RoundTrip("PresenceSub", Json(R"({"users":["a","","b"]})"))["users"].toArray(), (QJsonArray{ u"a"_s, u""_s, u"b"_s }));
	QCOMPARE(RoundTrip("ChatView", Json(R"({"chat":"c","pins":[3,0,9]})"))["pins"].toArray(), (QJsonArray{ 3, 0, 9 }));
	// map with a string value
	QCOMPARE(RoundTrip("Entry", Json(R"({"id":"n","refs":{"post":"p6","reply":""}})"))["refs"].toObject(), Json(R"({"post":"p6","reply":""})"));
	// doubles
	const auto location = RoundTrip("Location", Json(R"({"lat":35.6892,"lng":51.389,"accuracy_m":12.5})"));
	QCOMPARE(location["lat"].toDouble(), 35.6892);
	QCOMPARE(location["lng"].toDouble(), 51.389);
	QCOMPARE(location["accuracy_m"].toDouble(), 12.5);
}

void TestProto::alwaysFields() {
	const auto card = RoundTrip("CardOut", Json(R"({"id":"u"})"));
	QVERIFY(card.contains(u"display_name"_s));
	QCOMPARE(card["display_name"].toString(), QString());
	const auto inbox = RoundTrip("ChatInboxResponse", QJsonObject());
	QVERIFY(inbox["chats"].isArray());
	const auto page = RoundTrip("Page", QJsonObject());
	QVERIFY(page["chat"].isObject());
	QVERIFY(page["chat"].toObject().contains(u"participants"_s)); // nested always-keys too
	QVERIFY(page["messages"].isArray());
	const auto sent = RoundTrip("ChatSendResponse", Json(R"({"message_id":"m"})"));
	QCOMPARE(sent["seq"].toInt(-1), 0);
	QCOMPARE(sent["created_at"].toInt(-1), 0);
}

void TestProto::methodNames() {
	QCOMPARE(Proto::NamesOfMethod(u"chat.send"_s).request, QByteArray("ChatSendRequest"));
	QCOMPARE(Proto::NamesOfMethod(u"chat.send"_s).response, QByteArray("ChatSendResponse"));
	QCOMPARE(Proto::NamesOfMethod(u"chat.draft.save"_s).request, QByteArray("ChatDraftSaveRequest"));
	QCOMPARE(Proto::NamesOfMethod(u"chat.delete_for_both"_s).response, QByteArray("ChatDeleteForBothResponse"));
	for (const auto method : { "chat.send", "chat.history", "chat.inbox", "sync.difference", "users.cards", "presence", "chat.read" }) {
		const auto names = Proto::NamesOfMethod(QString::fromLatin1(method));
		QVERIFY2(Schema::FindMessage(QLatin1StringView(names.request)), method);
		QVERIFY2(Schema::FindMessage(QLatin1StringView(names.response)), method);
	}
	// Every method of the table names the messages the gateway uses.
	for (auto i = 0; i != Schema::MethodsCount(); ++i) {
		const auto &method = Schema::Methods()[i];
		const auto names = Proto::NamesOfMethod(QString::fromLatin1(method.name));
		QCOMPARE(QByteArray(Schema::MessageAt(method.request)->name), names.request);
		QCOMPARE(QByteArray(Schema::MessageAt(method.response)->name), names.response);
	}
}

void TestProto::rowsAreNestedLists() {
	const auto markup = Json(R"({"inline_keyboard":[[{"text":"A","kind":"callback"},{"text":"B","kind":"url","url":"https://eleza.me"}],[{"text":"C","kind":"callback"}]]})");
	const auto bytes = Proto::Encode("ClientMarkup"_L1, markup);
	QVERIFY(bytes.has_value());
	// The wrapped spelling encodes to the same bytes.
	const auto wrapped = Json(R"({"inline_keyboard":[{"items":[{"text":"A","kind":"callback"},{"text":"B","kind":"url","url":"https://eleza.me"}]},{"items":[{"text":"C","kind":"callback"}]}]})");
	QCOMPARE(Proto::Encode("ClientMarkup"_L1, wrapped).value(), *bytes);
	const auto back = Proto::Decode("ClientMarkup"_L1, *bytes);
	QVERIFY(back.has_value());
	QCOMPARE(back->value("inline_keyboard"_L1).toArray(), markup["inline_keyboard"].toArray());
}

void TestProto::numericStringsAndLargeIntegers() {
	// protojson writes 64-bit integers as strings; both spellings encode alike.
	QCOMPARE(
		Proto::Encode("Ack"_L1, Json(R"({"pts":"150"})")).value(),
		Proto::Encode("Ack"_L1, Json(R"({"pts":150})")).value());
	QCOMPARE(Proto::Encode("Ack"_L1, Json(R"({"pts":"9223372036854775807"})")).value(), QByteArray::fromHex("08ffffffffffffffff7f"));
	// 2^53 survives the trip through a JSON number.
	QCOMPARE(qint64(RoundTrip("Ack", Json(R"({"pts":9007199254740992})"))["pts"].toDouble()), qint64(9007199254740992));
	// numbers given for string fields (limit / before of chat.history) become their text
	const auto history = RoundTrip("ChatHistoryRequest", Json(R"({"chat":"c","before":120,"limit":50})"));
	QCOMPARE(history["before"].toString(), u"120"_s);
	QCOMPARE(history["limit"].toString(), u"50"_s);
	// uint32 wraps like the wire type
	QCOMPARE(RoundTrip("Ping", Json(R"({"nonce":4294967295})"))["nonce"].toDouble(), 4294967295.);
}

void TestProto::unknownFieldsAndMalformedInput() {
	// Unknown field numbers of every wire type are skipped.
	auto bytes = Proto::Encode("Ack"_L1, Json(R"({"pts":5})")).value();
	bytes.append(QByteArray::fromHex("f80101"));               // field 31 varint
	bytes.append(QByteArray::fromHex("f1010102030405060708")); // field 30 fixed64
	bytes.append(QByteArray::fromHex("f201026869"));           // field 30 bytes "hi"
	bytes.append(QByteArray::fromHex("f50101020304"));         // field 30 fixed32
	const auto back = Proto::Decode("Ack"_L1, bytes);
	QVERIFY(back.has_value());
	QCOMPARE(back->value("pts"_L1).toInt(), 5);

	QVERIFY(!Proto::Decode("Ack"_L1, QByteArray::fromHex("08")).has_value());       // truncated varint
	QVERIFY(!Proto::Decode("Request"_L1, QByteArray::fromHex("1205")).has_value()); // truncated bytes
	QVERIFY(!Proto::Decode("Ack"_L1, QByteArray::fromHex("0b")).has_value());       // wire type 3
	QVERIFY(!Proto::Decode("Ack"_L1, QByteArray::fromHex("08ffffffffffffffffffff01")).has_value()); // varint too long
	// A known field with the wrong wire type is skipped, not misread.
	const auto wrong = Proto::Decode("Ack"_L1, QByteArray::fromHex("0a026869"));
	QVERIFY(wrong.has_value());
	QCOMPARE(wrong->value("pts"_L1).toInt(-1), 0);
	// Recursion is bounded (Batch -> Envelope -> Batch ...).
	auto nested = QByteArray();
	for (auto i = 0; i != 60; ++i) {
		auto envelope = QByteArray::fromHex("52"); // Envelope.batch (10, length-delimited)
		auto batch = QByteArray();
		if (!nested.isEmpty()) {
			batch.append(char(0x0a));
			Q_ASSERT(nested.size() < 0x4000);
			if (nested.size() < 0x80) {
				batch.append(char(nested.size()));
			} else {
				batch.append(char((nested.size() & 0x7F) | 0x80));
				batch.append(char(nested.size() >> 7));
			}
			batch.append(nested);
		}
		Q_ASSERT(batch.size() < 0x4000);
		if (batch.size() < 0x80) {
			envelope.append(char(batch.size()));
		} else {
			envelope.append(char((batch.size() & 0x7F) | 0x80));
			envelope.append(char(batch.size() >> 7));
		}
		envelope.append(batch);
		nested = envelope;
	}
	QVERIFY(!Proto::Decode("Envelope"_L1, nested).has_value());
}

void TestProto::unwrapConventions() {
	QCOMPARE(Proto::UnwrapBody(Json(R"({"body":{"chat":"c1"}})")).toObject(), Json(R"({"chat":"c1"})"));
	QCOMPARE(Proto::UnwrapBody(Json(R"({"chats":[],"next_cursor":""})")).toObject(), Json(R"({"chats":[],"next_cursor":""})"));
	QCOMPARE(Proto::UnwrapBody(Json(R"({"body":"text"})")).toObject(), Json(R"({"body":"text"})"));
	QCOMPARE(Proto::UnwrapData(Json(R"({"read":{"chat":"c","seq":4}})")), Json(R"({"chat":"c","seq":4})"));
	QCOMPARE(Proto::UnwrapData(Json(R"({"chat":"c","seq":4})")), Json(R"({"chat":"c","seq":4})"));
	QCOMPARE(Proto::DataFieldOfKind(u"edited"_s), u"message"_s);
	QCOMPARE(Proto::DataFieldOfKind(u"message_unpinned"_s), u"pin"_s);
	QCOMPARE(Proto::DataFieldOfKind(u"notif"_s), u"notif"_s);
	QCOMPARE(Proto::DataFieldOfKind(u"dialog_pinned"_s), QString());

	// chat.header answers { body: ChatView }: the HTTP shape comes back.
	const auto header = Proto::Encode("ChatHeaderResponse"_L1, Json(R"({"body":{"chat":"c1","kind":"direct","participants":[{"ref":"u1","me":true}]}})"));
	const auto decoded = Proto::Decode("ChatHeaderResponse"_L1, header.value());
	const auto view = Proto::UnwrapBody(decoded.value()).toObject();
	QCOMPARE(view["chat"].toString(), u"c1"_s);
	QCOMPARE(view["participants"].toArray().at(0).toObject()["me"].toBool(), true);
	QCOMPARE(view["can_send"].toBool(true), false); // always-key restored with its zero value
}

void TestProto::updateFrameShapes() {
	// A live frame: data is the UpdateData oneof by kind.
	const auto live = Json(R"({"update":{"pts":101,"kind":"read","section":"message","data":{"read":{"chat":"c1","reader":"u_sara","seq":7}}}})");
	const auto liveBack = RoundTrip("Envelope", live)["update"].toObject();
	QCOMPARE(liveBack["pts"].toInt(), 101);
	QCOMPARE(Proto::UnwrapData(liveBack["data"].toObject()), Json(R"({"chat":"c1","reader":"u_sara","seq":7})"));
	QVERIFY(!liveBack.contains(u"chat"_s));
	// A replayed difference item: chat / seq / message / chat_info at the top.
	const auto replayed = Json(R"({"update":{"pts":102,"kind":"message","chat":"c1","seq":8,
		"message":{"chat":"c1","seq":8,"id":"m8","sender":"u_sara","kind":"text","text":"hi","created_at":1700000000000}}})");
	const auto replayedBack = RoundTrip("Envelope", replayed)["update"].toObject();
	QCOMPARE(replayedBack["chat"].toString(), u"c1"_s);
	QCOMPARE(replayedBack["message"].toObject()["text"].toString(), u"hi"_s);
	// notif: a zero `news` is omitted on the wire but restored (always-key).
	const auto notif = RoundTrip("Envelope", Json(R"({"update":{"pts":103,"kind":"notif","data":{"notif":{"news":0,"kinds":{"follow_request":1}}}}})"))["update"].toObject();
	const auto data = notif["data"].toObject()["notif"].toObject();
	QCOMPARE(data["news"].toInt(-1), 0);
	QCOMPARE(data["kinds"].toObject()["follow_request"].toInt(), 1);
	// batch of envelopes
	const auto batch = RoundTrip("Envelope", Json(R"({"batch":{"items":[{"ping":{"nonce":7}},{"signal":{"t":"resumed"}}]}})"));
	QCOMPARE(batch["batch"].toObject()["items"].toArray().size(), 2);
	QCOMPARE(batch["batch"].toObject()["items"].toArray().at(1).toObject()["signal"].toObject()["t"].toString(), u"resumed"_s);
}

void TestProto::httpBridge() {
	using Methods::HttpCall;
	auto call = Methods::ToHttp(u"chat.send"_s, Json(R"({"chat":"c 1","client_message_id":"x","kind":"text","text":"hi","reply_to_seq":3})"));
	QVERIFY(call.has_value());
	QCOMPARE(call->verb, u"POST"_s);
	QCOMPARE(call->path, u"/v1/chats/c%201/messages"_s);
	QVERIFY(call->query.isEmpty());
	QCOMPARE(call->body, Json(R"({"client_message_id":"x","kind":"text","text":"hi","reply_to_seq":3})"));
	QVERIFY(call->hasBody);

	call = Methods::ToHttp(u"chat.history"_s, Json(R"({"chat":"c1","before":"120","limit":"50","around":""})"));
	QVERIFY(call.has_value());
	QCOMPARE(call->verb, u"GET"_s);
	QCOMPARE(call->path, u"/v1/chats/c1/messages"_s);
	QCOMPARE(call->query, (QVector<QPair<QString, QString>>{ { u"before"_s, u"120"_s }, { u"limit"_s, u"50"_s } }));
	QVERIFY(!call->hasBody);

	call = Methods::ToHttp(u"chat.delete"_s, Json(R"({"chat":"c1","seq":12,"scope":"everyone"})"));
	QCOMPARE(call->verb, u"DELETE"_s);
	QCOMPARE(call->path, u"/v1/chats/c1/messages/12"_s);
	QCOMPARE(call->query, (QVector<QPair<QString, QString>>{ { u"scope"_s, u"everyone"_s } }));
	QVERIFY(!call->hasBody);

	call = Methods::ToHttp(u"chat.react"_s, Json(R"({"chat":"c1","seq":5,"emoji":"👍"})"));
	QCOMPARE(call->verb, u"PUT"_s);
	QCOMPARE(call->path, u"/v1/chats/c1/messages/5/reaction"_s);
	QCOMPARE(call->body, Json(R"({"emoji":"👍"})"));

	call = Methods::ToHttp(u"sync.difference"_s, Json(R"({"pts":"100"})"));
	QCOMPARE(call->path, u"/v1/sync/difference"_s);
	QCOMPARE(call->query, (QVector<QPair<QString, QString>>{ { u"pts"_s, u"100"_s } }));

	call = Methods::ToHttp(u"users.cards"_s, Json(R"({"ids":"a,b"})"));
	QCOMPARE(call->path, u"/v1/users"_s);
	QCOMPARE(call->query, (QVector<QPair<QString, QString>>{ { u"ids"_s, u"a,b"_s } }));

	call = Methods::ToHttp(u"chat.thread"_s, Json(R"({"user_id":"u1"})"));
	QCOMPARE(call->path, u"/v1/threads"_s);
	QCOMPARE(call->body, Json(R"({"user_id":"u1"})"));

	// A POST without fields still carries an (empty) JSON body, like the gateway's bridge.
	call = Methods::ToHttp(u"chat.accept"_s, Json(R"({"chat":"c1"})"));
	QCOMPARE(call->path, u"/v1/chats/c1/accept"_s);
	QVERIFY(call->hasBody);
	QVERIFY(call->body.isEmpty());

	QVERIFY(!Methods::ToHttp(u"chat.send"_s, Json(R"({"text":"no chat"})")).has_value()); // missing path parameter
	QVERIFY(!Methods::ToHttp(u"nope.method"_s, QJsonObject()).has_value());
	QVERIFY(Methods::Known(u"chat.draft.save"_s));
	QVERIFY(!Methods::Known(u"auth"_s));
	// Every method of the table can be bridged.
	for (auto i = 0; i != Schema::MethodsCount(); ++i) {
		const auto &method = Schema::Methods()[i];
		auto body = QJsonObject();
		const auto request = Schema::MessageAt(method.request);
		for (auto j = 0; j != request->count; ++j) {
			if (request->fields[j].flags & Schema::kPath) {
				body.insert(QLatin1StringView(request->fields[j].json), u"x"_s);
			}
		}
		const auto bridged = Methods::ToHttp(QString::fromLatin1(method.name), body);
		QVERIFY2(bridged.has_value(), method.name);
		QVERIFY2(!bridged->path.contains(u'{'), method.name);
	}
}

void TestProto::hostileBytes() {
	const auto decode = [](const char *message, const char *hex) {
		return Proto::Decode(QLatin1StringView(message), QByteArray::fromHex(hex));
	};
	// Field numbers are 1 .. 2^29 - 1: zero and anything above is not protobuf
	// (and must never alias a real field after a narrowing cast).
	QVERIFY(!decode("Close", "0005"));                    // field 0
	QVERIFY(!decode("Close", "88808080800105"));          // field 2^32 + 1: must not alias field 1
	QVERIFY(!decode("Close", "808080801005"));            // field 2^29
	QVERIFY(decode("Close", "f8ffffff0f05"));             // field 2^29 - 1: unknown, skipped
	// Varints: eleven bytes, or cut in the middle.
	QVERIFY(!decode("Close", "08ffffffffffffffffffff01"));
	QVERIFY(!decode("Close", "08ff"));
	// Lengths past the end of the buffer, including 2^64 - 1.
	QVERIFY(!decode("Close", "12ffffffffffffffffff01"));
	QVERIFY(!decode("Close", "1205616263"));
	// Groups (wire types 3 and 4) and the unused 6 / 7.
	for (const auto hex : { "0b", "0c", "0e", "0f" }) {
		QVERIFY2(!decode("Close", hex), hex);
	}
	// An int64 at the edges survives the trip through a JSON number without
	// leaving the range of qint64 (2^63 - 1 is not a double).
	const auto edge = decode("Hello", "20ffffffffffffffff7f");
	QVERIFY(edge.has_value());
	// Invalid UTF-8 in a string field becomes replacement characters.
	const auto text = decode("Close", "1204ff41c328");
	QVERIFY(text.has_value());
	QVERIFY(text->value(QLatin1StringView("reason")).toString().contains(QChar(0xFFFD)));
	// Nesting deeper than the limit: Envelope.batch.items[0].batch.items[0]...
	auto nested = QByteArray();
	const auto envelope = Schema::FindMessage(QLatin1StringView("Envelope"));
	QVERIFY(envelope);
	auto deep = QJsonObject{ { u"ping"_s, QJsonObject{ { u"nonce"_s, 1 } } } };
	for (auto i = 0; i != 40; ++i) {
		deep = QJsonObject{ { u"batch"_s, QJsonObject{ { u"items"_s, QJsonArray{ deep } } } } };
	}
	nested = Proto::Encode(*envelope, deep);
	QVERIFY(!Proto::Decode(*envelope, nested));
}

void TestProto::repeatedFieldsAreLinearAndBudgeted() {
	// The largest envelope the framing lets through, filled with as many
	// elements of one repeated field as fit: the decode must take time linear
	// in the input (it used to copy the growing list for every element: tens
	// of seconds on the GUI thread for one frame) and must not turn two-byte
	// elements into an unbounded amount of JSON.
	const auto varint = [](quint64 value) {
		auto result = QByteArray();
		while (value >= 0x80) {
			result.append(char((value & 0x7F) | 0x80));
			value >>= 7;
		}
		result.append(char(value));
		return result;
	};
	constexpr auto kFrame = 256 << 10;
	auto slowest = qint64(0);
	auto slowestName = QByteArray();
	auto checked = 0;
	auto refused = 0;
	for (auto i = 0; i != Schema::MessagesCount(); ++i) {
		const auto &message = Schema::Messages()[i];
		for (auto j = 0; j != message.count; ++j) {
			const auto &field = message.fields[j];
			const auto map = (field.kind == Schema::Kind::Map);
			if (!field.repeated() && !map) {
				continue;
			}
			// Length-delimited and empty (an empty message, string or map
			// entry), or a one-byte varint for the packed kinds sent unpacked.
			const auto delimited = map
				|| field.kind == Schema::Kind::Message
				|| field.kind == Schema::Kind::String
				|| field.kind == Schema::Kind::Bytes;
			if (!delimited && field.kind == Schema::Kind::Double) {
				continue;
			}
			const auto item = varint((quint64(field.number) << 3) | (delimited ? 2 : 0))
				+ QByteArray(1, char(delimited ? 0 : 1));
			auto bytes = QByteArray();
			bytes.reserve(kFrame);
			while (bytes.size() + item.size() <= kFrame) {
				bytes.append(item);
			}
			auto timer = QElapsedTimer();
			timer.start();
			const auto decoded = Proto::Decode(message, bytes);
			const auto took = timer.elapsed();
			++checked;
			if (!decoded) {
				++refused;
			}
			if (took > slowest) {
				slowest = took;
				slowestName = QByteArray(message.name) + '.' + field.json;
			}
		}
	}
	qInfo("%d repeated fields, %d refused by the budget, slowest %s: %lld ms",
		checked, refused, slowestName.constData(), qlonglong(slowest));
	QVERIFY(checked > 20);
	// Generous for an unoptimized build under a sanitizer; the quadratic
	// version needed minutes there.
	QVERIFY2(slowest < 8000, slowestName.constData());
}

QTEST_GUILESS_MAIN(TestProto)

#include "test_proto.moc"
