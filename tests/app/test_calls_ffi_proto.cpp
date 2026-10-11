// The protobuf wire codec of the media library's messages
// (calls/calls_ffi_proto.h): known encodings byte for byte, round trips, and
// malformed input that must stop the reader instead of running off the end.
#include "calls/calls_ffi_proto.h"

#include <QtTest/QtTest>

using Calls::Ffi::Reader;
using Calls::Ffi::Writer;

class TestCallsFfiProto final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void knownEncodings();
	void roundTrip();
	void requiredZeroValuesAreWritten();
	void nestedMessages();
	void pointersStayExact();
	void unknownFieldsAreSkipped();
	void malformed_data();
	void malformed();
	void emptyInput();

};

void TestCallsFfiProto::knownEncodings() {
	// The examples of the format's documentation.
	QCOMPARE(Writer().varint(1, 150).data(), QByteArray::fromHex("089601"));
	QCOMPARE(
		Writer().string(2, QStringLiteral("testing")).data(),
		QByteArray::fromHex("120774657374696e67"));
	auto inner = Writer();
	inner.varint(1, 150);
	QCOMPARE(Writer().message(3, inner).data(), QByteArray::fromHex("1a03089601"));
	// A negative int32 takes ten bytes.
	QCOMPARE(
		Writer().integer(1, -1).data(),
		QByteArray::fromHex("08ffffffffffffffffff01"));
	QCOMPARE(
		Writer().real(4, 1.0).data(),
		QByteArray::fromHex("21000000000000f03f"));
	// Field numbers above 15 take a two-byte key.
	QCOMPARE(Writer().boolean(16, true).data(), QByteArray::fromHex("800101"));
}

void TestCallsFfiProto::roundTrip() {
	auto writer = Writer();
	writer.varint(1, 0)
		.varint(2, 127)
		.varint(3, 128)
		.varint(4, 0xFFFFFFFFFFFFFFFFULL)
		.integer(5, -42)
		.boolean(6, true)
		.real(7, -0.125)
		.string(8, QString::fromUtf8("سلام wss://sfu.example"))
		.bytes(9, QByteArray("\0\x01\xff", 3))
		.bytes(10, QByteArray());
	const auto data = writer.data();
	auto reader = Reader(data);
	auto seen = 0;
	while (reader.next()) {
		++seen;
		switch (reader.field()) {
		case 1: QCOMPARE(reader.varint(), quint64(0)); break;
		case 2: QCOMPARE(reader.varint(), quint64(127)); break;
		case 3: QCOMPARE(reader.varint(), quint64(128)); break;
		case 4: QCOMPARE(reader.varint(), 0xFFFFFFFFFFFFFFFFULL); break;
		case 5: QCOMPARE(reader.integer(), qint64(-42)); break;
		case 6: QVERIFY(reader.boolean()); break;
		case 7: QCOMPARE(reader.real(), -0.125); break;
		case 8:
			QVERIFY(reader.isChunk());
			QCOMPARE(reader.string(), QString::fromUtf8("سلام wss://sfu.example"));
			break;
		case 9: QCOMPARE(reader.bytes(), QByteArray("\0\x01\xff", 3)); break;
		case 10:
			QVERIFY(reader.isChunk());
			QVERIFY(reader.bytes().isEmpty());
			break;
		default: QFAIL("an unexpected field");
		}
	}
	QVERIFY(reader.ok());
	QCOMPARE(seen, 10);
}

void TestCallsFfiProto::requiredZeroValuesAreWritten() {
	// proto2 `required`: a zero is on the wire, not left out.
	const auto data = Writer().varint(1, 0).boolean(2, false).string(3, QString()).data();
	QCOMPARE(data, QByteArray::fromHex("080010001a00"));
}

void TestCallsFfiProto::nestedMessages() {
	auto server = Writer();
	server.string(1, QStringLiteral("turn:turn.example:3478?transport=udp"))
		.string(2, QStringLiteral("user"))
		.string(3, QStringLiteral("secret"));
	auto rtc = Writer();
	rtc.integer(1, 1).message(3, server).message(3, server);
	auto options = Writer();
	options.boolean(1, true).message(6, rtc);
	const auto data = Writer()
		.string(1, QStringLiteral("ws://127.0.0.1:7880"))
		.message(3, options)
		.data();

	auto urls = QStringList();
	auto reader = Reader(data);
	while (reader.next()) {
		if (reader.field() != 3) {
			continue;
		}
		auto optionsReader = reader.sub();
		while (optionsReader.next()) {
			if (optionsReader.field() != 6) {
				continue;
			}
			auto rtcReader = optionsReader.sub();
			while (rtcReader.next()) {
				if (rtcReader.field() != 3) {
					continue;
				}
				auto serverReader = rtcReader.sub();
				while (serverReader.next()) {
					if (serverReader.field() == 1) {
						urls.push_back(serverReader.string());
					}
				}
				QVERIFY(serverReader.ok());
			}
			QVERIFY(rtcReader.ok());
		}
		QVERIFY(optionsReader.ok());
	}
	QVERIFY(reader.ok());
	QCOMPARE(urls.size(), 2);
	QCOMPARE(urls[0], QStringLiteral("turn:turn.example:3478?transport=udp"));
}

void TestCallsFfiProto::pointersStayExact() {
	// Audio buffers travel as addresses in uint64 fields.
	const auto pointer = quint64(0x00007FFDEADBEEF8ULL);
	const auto data = Writer().varint(1, pointer).varint(2, quint64(1) << 63).data();
	auto reader = Reader(data);
	QVERIFY(reader.next());
	QCOMPARE(reader.varint(), pointer);
	QVERIFY(reader.next());
	QCOMPARE(reader.varint(), quint64(1) << 63);
	QVERIFY(!reader.next());
	QVERIFY(reader.ok());
}

void TestCallsFfiProto::unknownFieldsAreSkipped() {
	// A 32-bit field (wire type 5) is never written here but may arrive.
	auto data = Writer().varint(1, 7).data();
	data.append(QByteArray::fromHex("150000803f")); // field 2, float 1.0
	data.append(Writer().string(3, QStringLiteral("after")).data());
	auto reader = Reader(data);
	QVERIFY(reader.next());
	QCOMPARE(reader.field(), 1);
	QVERIFY(reader.next());
	QCOMPARE(reader.field(), 2);
	QCOMPARE(reader.real(), 0.); // not a double
	QVERIFY(reader.next());
	QCOMPARE(reader.string(), QStringLiteral("after"));
	QVERIFY(!reader.next());
	QVERIFY(reader.ok());
}

void TestCallsFfiProto::malformed_data() {
	QTest::addColumn<QByteArray>("data");

	QTest::newRow("a key without a value") << QByteArray::fromHex("08");
	QTest::newRow("a varint that never ends")
		<< QByteArray::fromHex("08ffffffffffffffffffffffff");
	QTest::newRow("a varint cut in the middle") << QByteArray::fromHex("0896");
	QTest::newRow("a length beyond the end") << QByteArray::fromHex("120a6162");
	QTest::newRow("a huge length") << QByteArray::fromHex("12ffffffffffffffff7f");
	QTest::newRow("a short 64-bit value") << QByteArray::fromHex("2100000000");
	QTest::newRow("a short 32-bit value") << QByteArray::fromHex("150000");
	QTest::newRow("a group (not supported)") << QByteArray::fromHex("0b");
	QTest::newRow("wire type 7") << QByteArray::fromHex("0f00");
}

void TestCallsFfiProto::malformed() {
	QFETCH(QByteArray, data);

	auto reader = Reader(data);
	auto steps = 0;
	while (reader.next()) {
		QVERIFY(++steps < 16);
	}
	QVERIFY(!reader.ok());
	// And it stays stopped.
	QVERIFY(!reader.next());
}

void TestCallsFfiProto::emptyInput() {
	auto reader = Reader(QByteArray());
	QVERIFY(!reader.next());
	QVERIFY(reader.ok());
	auto null = Reader(nullptr, 0);
	QVERIFY(!null.next());
	QVERIFY(null.ok());
	// A scalar field has no nested message.
	const auto data = Writer().varint(1, 5).data();
	auto scalar = Reader(data);
	QVERIFY(scalar.next());
	auto sub = scalar.sub();
	QVERIFY(!sub.next());
	QVERIFY(sub.ok());
	QVERIFY(scalar.bytes().isEmpty());
}

QTEST_APPLESS_MAIN(TestCallsFfiProto)
#include "test_calls_ffi_proto.moc"
