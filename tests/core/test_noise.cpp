// Noise NK against the backend's vectors (shared with Android, iOS and the web client),
// tamper detection, and the record framing with padding, chunking and rekeying.
#include "base/bytes.h"
#include "crypto/crypto_primitives.h"
#include "rt/rt_frames.h"
#include "rt/rt_noise.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtTest/QtTest>

using namespace Rt;

namespace {

[[nodiscard]] QByteArray Hex(const QJsonObject &vector, const char *key) {
	return base::bytes::fromHex(vector.value(QLatin1StringView(key)).toString());
}

[[nodiscard]] QString Text(const QByteArray &bytes) {
	return base::bytes::toHex(bytes);
}

struct Pair {
	Noise::Session client;
	Noise::Session server;
};

[[nodiscard]] Pair Handshake(const QByteArray &prologue) {
	const auto staticPrivate = Crypto::X25519GeneratePrivate();
	auto initiator = Noise::Initiator(Crypto::X25519PublicFromPrivate(staticPrivate), prologue);
	auto responder = Noise::Responder(staticPrivate, prologue);
	const auto message1 = initiator.writeMessage1(QByteArray());
	Q_ASSERT(message1);
	const auto payload1 = responder.readMessage1(*message1);
	Q_ASSERT(payload1);
	const auto message2 = responder.writeMessage2(QByteArray());
	Q_ASSERT(message2);
	const auto payload2 = initiator.readMessage2(*message2);
	Q_ASSERT(payload2);
	return { initiator.session(), responder.session() };
}

} // namespace

class TestNoise final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void backendVectors();
	void tamperingAndAssociatedData();
	void rejectsBadInput();
	void framesPadChunkAndReassemble();
	void framesEmptyAndLimits();
	void framesRekeyEvery65536();
	void framesRejectMalformed();

};

void TestNoise::backendVectors() {
	auto file = QFile(QStringLiteral(ELEZA_TESTDATA_DIR "/nk_sha256_vectors.json"));
	QVERIFY(file.open(QIODevice::ReadOnly));
	const auto vectors = QJsonDocument::fromJson(file.readAll()).array();
	QVERIFY(!vectors.isEmpty());
	for (const auto &value : vectors) {
		const auto v = value.toObject();
		const auto staticPrivate = Hex(v, "server_static_priv");
		QCOMPARE(Text(Crypto::X25519PublicFromPrivate(staticPrivate)), v["server_static_pub"].toString());
		const auto prologue = Hex(v, "prologue");

		auto initiator = Noise::Initiator(Hex(v, "server_static_pub"), prologue, Hex(v, "client_eph_priv"));
		QVERIFY(initiator.valid());
		const auto message1 = initiator.writeMessage1(Hex(v, "payload1"));
		QVERIFY(message1.has_value());
		QCOMPARE(Text(*message1), v["msg1"].toString());

		auto responder = Noise::Responder(staticPrivate, prologue, Hex(v, "server_eph_priv"));
		const auto payload1 = responder.readMessage1(*message1);
		QVERIFY(payload1.has_value());
		QCOMPARE(Text(*payload1), v["payload1"].toString());
		const auto message2 = responder.writeMessage2(Hex(v, "payload2"));
		QVERIFY(message2.has_value());
		QCOMPARE(Text(*message2), v["msg2"].toString());

		const auto payload2 = initiator.readMessage2(*message2);
		QVERIFY(payload2.has_value());
		QCOMPARE(Text(*payload2), v["payload2"].toString());
		QVERIFY(initiator.established());

		auto &client = initiator.session();
		auto &server = responder.session();
		QCOMPARE(Text(client.handshakeHash), v["handshake_hash"].toString());
		QCOMPARE(Text(server.handshakeHash), v["handshake_hash"].toString());

		const auto transport1 = client.send.encrypt(QByteArray(), QByteArray("ping"));
		QVERIFY(transport1.has_value());
		QCOMPARE(Text(*transport1), v["transport_c2s_1"].toString());
		QCOMPARE(server.recv.decrypt(QByteArray(), *transport1).value_or(QByteArray()), QByteArray("ping"));
		const auto transport2 = server.send.encrypt(QByteArray(), QByteArray("pong"));
		QVERIFY(transport2.has_value());
		QCOMPARE(Text(*transport2), v["transport_s2c_1"].toString());
		QCOMPARE(client.recv.decrypt(QByteArray(), *transport2).value_or(QByteArray()), QByteArray("pong"));
	}
}

void TestNoise::tamperingAndAssociatedData() {
	auto pair = Handshake(QByteArray("p"));
	const auto sealed = pair.client.send.encrypt(QByteArray(), QByteArray("a"));
	QVERIFY(sealed.has_value());
	auto bad = *sealed;
	bad[0] = char(bad[0] ^ 1);
	// A failed decryption must not advance the nonce: the genuine record still opens.
	QVERIFY(!pair.server.recv.decrypt(QByteArray(), bad).has_value());
	QCOMPARE(pair.server.recv.decrypt(QByteArray(), *sealed).value_or(QByteArray()), QByteArray("a"));

	const auto bound = pair.client.send.encrypt(QByteArray("ad"), QByteArray("b"));
	QVERIFY(bound.has_value());
	QVERIFY(!pair.server.recv.decrypt(QByteArray("other"), *bound).has_value());
	QCOMPARE(pair.server.recv.decrypt(QByteArray("ad"), *bound).value_or(QByteArray()), QByteArray("b"));
}

void TestNoise::rejectsBadInput() {
	auto shortKey = Noise::Initiator(QByteArray(31, 'x'), QByteArray("p"));
	QVERIFY(!shortKey.valid());
	QVERIFY(!shortKey.writeMessage1(QByteArray("x")).has_value());

	const auto staticPrivate = Crypto::X25519GeneratePrivate();
	auto initiator = Noise::Initiator(Crypto::X25519PublicFromPrivate(staticPrivate), QByteArray("p"));
	QVERIFY(!initiator.readMessage2(QByteArray(64, 'x')).has_value()); // before message 1
	QVERIFY(initiator.writeMessage1(QByteArray("x")).has_value());
	QVERIFY(!initiator.writeMessage1(QByteArray("x")).has_value()); // twice
	QVERIFY(!initiator.readMessage2(QByteArray(10, 'x')).has_value()); // short
	// A responder with another static key cannot answer: wrong prologue binds too.
	auto responder = Noise::Responder(staticPrivate, QByteArray("q"));
	auto other = Noise::Initiator(Crypto::X25519PublicFromPrivate(staticPrivate), QByteArray("p"));
	const auto message1 = other.writeMessage1(QByteArray("hello"));
	QVERIFY(message1.has_value());
	QVERIFY(!responder.readMessage1(*message1).has_value());
}

void TestNoise::framesPadChunkAndReassemble() {
	auto pair = Handshake(QByteArray("eleza-rt-v2/1"));
	auto decoder = Frames::RecordDecoder(&pair.server.recv);

	const auto small = QByteArray("hello");
	const auto records = Frames::EncodeRecords(pair.client.send, small);
	QVERIFY(records.has_value());
	QCOMPARE(records->size(), 1);
	QCOMPARE((records->at(0).size() - Noise::kTagSize) % Frames::kPadding, 0);
	QCOMPARE(records->at(0).size(), 64 + Noise::kTagSize);
	const auto got = decoder.record(records->at(0));
	QCOMPARE(got.status, Frames::RecordDecoder::Status::Complete);
	QCOMPARE(got.envelope, small);

	// 58 bytes + 6 header = exactly one block; 59 spills into the second.
	QCOMPARE(Frames::EncodeRecords(pair.client.send, QByteArray(58, 'a'))->at(0).size(), 64 + 16);
	QCOMPARE(Frames::EncodeRecords(pair.client.send, QByteArray(59, 'a'))->at(0).size(), 128 + 16);

	auto pair2 = Handshake(QByteArray("eleza-rt-v2/1"));
	auto decoder2 = Frames::RecordDecoder(&pair2.server.recv);
	auto big = QByteArray(200000, Qt::Uninitialized);
	for (auto i = 0; i != big.size(); ++i) {
		big[i] = char((i * 31 + 7) & 0xFF);
	}
	const auto parts = Frames::EncodeRecords(pair2.client.send, big);
	QVERIFY(parts.has_value());
	QVERIFY(parts->size() > 3);
	auto result = Frames::RecordDecoder::Result();
	for (auto i = 0; i != parts->size(); ++i) {
		QVERIFY(parts->at(i).size() <= Noise::kMaxMessage);
		result = decoder2.record(parts->at(i));
		QCOMPARE(result.status, (i + 1 == parts->size())
			? Frames::RecordDecoder::Status::Complete
			: Frames::RecordDecoder::Status::Partial);
	}
	QCOMPARE(result.envelope, big);

	// The other direction uses the other pair of keys.
	auto back = Frames::RecordDecoder(&pair2.client.recv);
	const auto reply = Frames::EncodeRecords(pair2.server.send, QByteArray("pong"));
	QCOMPARE(back.record(reply->at(0)).envelope, QByteArray("pong"));
}

void TestNoise::framesEmptyAndLimits() {
	auto pair = Handshake(QByteArray("x"));
	auto decoder = Frames::RecordDecoder(&pair.server.recv);
	const auto empty = Frames::EncodeRecords(pair.client.send, QByteArray());
	QVERIFY(empty.has_value());
	QCOMPARE(empty->size(), 1);
	const auto got = decoder.record(empty->at(0));
	QCOMPARE(got.status, Frames::RecordDecoder::Status::Complete);
	QVERIFY(got.envelope.isEmpty());

	QVERIFY(!Frames::EncodeRecords(pair.client.send, QByteArray(Frames::kMaxFrame + 1, 'x')).has_value());
	const auto max = Frames::EncodeRecords(pair.client.send, QByteArray(Frames::kMaxFrame, 'x'));
	QVERIFY(max.has_value());
	auto last = Frames::RecordDecoder::Result();
	for (const auto &record : *max) {
		last = decoder.record(record);
	}
	QCOMPARE(last.status, Frames::RecordDecoder::Status::Complete);
	QCOMPARE(last.envelope.size(), Frames::kMaxFrame);
}

void TestNoise::framesRekeyEvery65536() {
	auto pair = Handshake(QByteArray("rekey"));
	auto decoder = Frames::RecordDecoder(&pair.server.recv);
	const auto total = int(Frames::kRekeyEvery) + 5;
	for (auto i = 0; i != total; ++i) {
		const auto payload = QByteArray::number(i);
		const auto records = Frames::EncodeRecords(pair.client.send, payload);
		QVERIFY(records.has_value());
		const auto got = decoder.record(records->at(0));
		QVERIFY2(got.status == Frames::RecordDecoder::Status::Complete, qPrintable(QString::number(i)));
		QCOMPARE(got.envelope, payload);
		if (i + 1 == int(Frames::kRekeyEvery)) {
			// Both sides rekeyed after the 65536th record and reset their counters.
			QCOMPARE(pair.client.send.count(), 0U);
			QCOMPARE(pair.server.recv.count(), 0U);
		}
	}
	QCOMPARE(pair.client.send.count(), 5U);
}

void TestNoise::framesRejectMalformed() {
	auto pair = Handshake(QByteArray("m"));
	auto decoder = Frames::RecordDecoder(&pair.server.recv);
	// Garbage does not authenticate.
	QCOMPARE(decoder.record(QByteArray(80, 'z')).status, Frames::RecordDecoder::Status::Failed);
	// A record whose chunk length exceeds its plaintext.
	auto plaintext = QByteArray(64, char(0));
	plaintext[0] = 1;
	plaintext[1] = char(0x10); // 4096 bytes claimed
	plaintext[5] = 5;
	const auto sealed = pair.client.send.encrypt(QByteArray(), plaintext);
	QCOMPARE(decoder.record(*sealed).status, Frames::RecordDecoder::Status::Failed);
	// A continuation whose total differs from the first chunk's.
	auto first = QByteArray(64, char(0));
	first[2] = 10;
	first[5] = 20; // total 20, chunk 10, not last
	auto second = QByteArray(64, char(0));
	second[0] = 1;
	second[2] = 10;
	second[5] = 21;
	QCOMPARE(decoder.record(*pair.client.send.encrypt(QByteArray(), first)).status, Frames::RecordDecoder::Status::Partial);
	QCOMPARE(decoder.record(*pair.client.send.encrypt(QByteArray(), second)).status, Frames::RecordDecoder::Status::Failed);
}

QTEST_GUILESS_MAIN(TestNoise)

#include "test_noise.moc"
