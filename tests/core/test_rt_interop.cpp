// Rt::Client end to end against an independent implementation of the
// protocol: tests/core/rtpeer, a gateway built from the web client's own
// Noise / framing / protobuf code (see rtpeer/run.mjs).
//
// Environment: ELEZA_NODE (the node binary, default: from PATH),
// ELEZA_PWA_DIR (checkout of the web client), ELEZA_RTPEER_DIR (rtpeer
// sources), ELEZA_RTPEER_WORK (scratch directory for its dependencies).
// Exits with 77 (skipped) when node, the web client or the peer's
// dependencies are not available.
#include "api/api_client.h"
#include "crypto/crypto_device_key.h"
#include "rt/rt_client.h"
#include "storage/storage_key_value.h"
#include "test_fakes.h"

#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtTest/QSignalSpy>

namespace {

using Type = Rt::Frame::Type;

QString PeerBase;

class Delegate final : public Rt::Client::Delegate {
public:
	QString token = u"tok-1"_s;
	QString tokenAfterRefresh;
	Crypto::DeviceKey key = Crypto::DeviceKey::Generate();
	int refreshes = 0;
	int revoked = 0;
	int proofs = 0;

	void accessToken(Fn<void(QString)> done) override {
		done(token);
	}
	QString currentAccessToken() override {
		return token;
	}
	void refreshAfterUnauthorized(const QString &failedToken, Fn<void(bool)> done) override {
		++refreshes;
		if (failedToken == token && !tokenAfterRefresh.isEmpty()) {
			token = tokenAfterRefresh;
		}
		done(true);
	}
	bool signedIn() override {
		return !token.isEmpty();
	}
	void sessionRevoked() override {
		++revoked;
		token = QString();
	}
	QByteArray proof(const QByteArray &nonce) override {
		++proofs;
		return key.sign(u"ws_resume"_s, nonce);
	}
	QString deviceId() override {
		return u"device-7"_s;
	}
};

// One client with everything it needs, and the frames it reported.
class Peer final {
public:
	Peer()
	: api(PeerBase, u"1.2.3"_s)
	, cache(dir.filePath(u"settings.json"_s))
	, client(&api, &cache, &delegate) {
		api.setLanguage(u"fa"_s);
		QObject::connect(&client, &Rt::Client::frameReceived, &client, [this](const Rt::Frame &frame) {
			frames.push_back(frame);
		});
	}

	[[nodiscard]] bool connect() {
		client.start();
		return Test::WaitFor([&] { return client.isConnected(); }, 15000);
	}

	// A control call to the peer (plain HTTP, synchronous for the test).
	QJsonObject control(Api::Method method, const QString &path, const QJsonObject &body = {}) {
		auto result = std::optional<QJsonObject>();
		auto request = api.request(method, path);
		if (method != Api::Get) {
			request.body(body);
		}
		request
			.unauthenticated()
			.done([&](const QJsonObject &json) { result = json; })
			.fail([&](const Api::Error &error) {
				result = QJsonObject{ { u"error"_s, error.describe() } };
			})
			.send();
		const auto answered = Test::WaitFor([&] { return result.has_value(); });
		Q_ASSERT(answered);
		Q_UNUSED(answered);
		return *result;
	}
	[[nodiscard]] QJsonObject state() {
		return control(Api::Get, u"/control/state"_s);
	}

	struct Answer {
		bool finished = false;
		bool ok = false;
		QJsonObject json;
		Api::Error error;
	};
	[[nodiscard]] Answer call(
			const QString &method,
			const QJsonObject &body = {},
			const QString &idempotencyKey = QString()) {
		auto answer = Answer();
		client.call(method, body, idempotencyKey, base::Guard(), [&](const QJsonValue &value) {
			answer.finished = answer.ok = true;
			answer.json = value.toObject();
		}, [&](const Api::Error &error) {
			answer.finished = true;
			answer.error = error;
		});
		const auto finished = Test::WaitFor([&] { return answer.finished; }, 15000);
		Q_UNUSED(finished);
		return answer;
	}

	[[nodiscard]] int count(Type type) const {
		return int(std::count_if(frames.cbegin(), frames.cend(), [&](const Rt::Frame &frame) {
			return frame.type == type;
		}));
	}
	[[nodiscard]] const Rt::Frame *last(Type type) const {
		for (auto i = frames.crbegin(); i != frames.crend(); ++i) {
			if (i->type == type) {
				return &*i;
			}
		}
		return nullptr;
	}
	[[nodiscard]] bool waitFor(Type type, int count = 1, int timeoutMs = 15000) {
		return Test::WaitFor([&] { return this->count(type) >= count; }, timeoutMs);
	}

	QTemporaryDir dir;
	Delegate delegate;
	Api::Client api;
	Storage::KeyValue cache;
	Rt::Client client;
	QVector<Rt::Frame> frames;

};

[[nodiscard]] QJsonObject LastOf(const QJsonObject &state, const char *list) {
	const auto array = state.value(QLatin1StringView(list)).toArray();
	return array.isEmpty() ? QJsonObject() : array.last().toObject();
}

[[nodiscard]] QJsonObject LastSignal(const QJsonObject &state, const QString &type) {
	const auto array = state.value("signals"_L1).toArray();
	for (auto i = array.size(); i-- > 0;) {
		if (array[i].toObject().value("t"_L1).toString() == type) {
			return array[i].toObject();
		}
	}
	return QJsonObject();
}

} // namespace

class TestRtInterop final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void handshakeAndHello();
	void rpc();
	void sendAndEcho();
	void pushedUpdate();
	void signalsAndSubscriptions();
	void reconnectResumes();
	void reauthClose();
	void expiredTokenProof();
	void keyRotation();
	void revoked();

};

void TestRtInterop::handshakeAndHello() {
	auto peer = Peer();
	auto states = QVector<Rt::State>();
	connect(&peer.client, &Rt::Client::stateChanged, &peer.client, [&](Rt::State state) {
		states.push_back(state);
	});
	QVERIFY(peer.client.state() == Rt::State::Stopped);
	QVERIFY(peer.connect());
	QVERIFY(peer.client.state() == Rt::State::Connected);
	QCOMPARE(states, (QVector<Rt::State>{ Rt::State::Connecting, Rt::State::Connected }));

	QCOMPARE(peer.frames.size(), 1);
	QVERIFY(peer.frames[0].type == Type::Hello);
	QVERIFY(!peer.frames[0].resumed);
	const auto state = peer.state();
	QCOMPARE(peer.frames[0].pts, qint64(state.value("pts"_L1).toDouble()));
	QCOMPARE(peer.client.hello().version, 2);
	QCOMPARE(peer.client.hello().pingInterval, 25);
	QCOMPARE(peer.client.hello().reconnectSpread, 5);
	QVERIFY(peer.client.hello().connection.startsWith(u"conn-"));
	QVERIFY(peer.client.hello().serverTimeMs > 1700000000000);

	// What the other side read from message 1.
	const auto auth = LastOf(state, "auths");
	QCOMPARE(auth.value("token"_L1).toString(), u"tok-1"_s);
	QCOMPARE(auth.value("device"_L1).toString(), u"device-7"_s);
	QCOMPARE(auth.value("app"_L1).toString(), peer.api.clientHeader());
	QCOMPARE(auth.value("lang"_L1).toString(), u"fa"_s);
	QVERIFY(!auth.contains("pts"_L1)); // nothing to resume from: the zero is not on the wire
	QCOMPARE(auth.value("key_id"_L1).toInt(), state.value("key_id"_L1).toInt());
	QCOMPARE(state.value("connections"_L1).toInt(), 1);
	// The key came from GET /v2/keys, not from the one built in.
	QVERIFY(state.value("rejectedKeyIds"_L1).toArray().isEmpty());

	peer.client.stop();
	QVERIFY(!peer.client.isConnected());
	QVERIFY(peer.client.state() == Rt::State::Stopped);
	QVERIFY(Test::WaitFor([&] { return peer.state().value("connections"_L1).toInt() == 0; }));
	const auto answer = peer.call(u"me.get"_s);
	QVERIFY(!answer.ok);
	QCOMPARE(answer.error.code, u"rt.not_open"_s);
	QCOMPARE(answer.error.status, 0);
}

void TestRtInterop::rpc() {
	auto peer = Peer();
	QVERIFY(peer.connect());

	const auto me = peer.call(u"me.get"_s);
	QVERIFY2(me.ok, qPrintable(me.error.describe()));
	QCOMPARE(me.json.value("id"_L1).toString(), u"u_me"_s);
	QCOMPARE(me.json.value("display_name"_L1).toString(), u"من"_s);
	QCOMPARE(me.json.value("followers"_L1).toInt(), 12);
	QCOMPARE(me.json.value("verified"_L1).toBool(), true);
	// Keys the JSON API always sends are there even at their zero value.
	QVERIFY(me.json.contains("bio"_L1));
	QCOMPARE(me.json.value("is_private"_L1).toBool(true), false);
	QVERIFY(!me.json.contains("avatar_id"_L1));

	// A `body` wrapper is unwrapped: the answer has the HTTP shape.
	const auto page = peer.call(u"chat.history"_s, {
		{ u"chat"_s, u"c1"_s },
		{ u"limit"_s, u"3"_s },
	});
	QVERIFY2(page.ok, qPrintable(page.error.describe()));
	const auto messages = page.json.value("messages"_L1).toArray();
	QCOMPARE(messages.size(), 3);
	QCOMPARE(messages[0].toObject().value("seq"_L1).toInt(), 3);
	QCOMPARE(messages[0].toObject().value("text"_L1).toString(), u"پیام 3"_s);
	QCOMPARE(messages[2].toObject().value("sender"_L1).toString(), u"u_sara"_s);
	QCOMPARE(qint64(messages[2].toObject().value("created_at"_L1).toDouble()), 1800000000001);
	const auto chat = page.json.value("chat"_L1).toObject();
	QCOMPARE(chat.value("chat"_L1).toString(), u"c1"_s);
	QCOMPARE(chat.value("participants"_L1).toArray().size(), 2);
	QCOMPARE(chat.value("participants"_L1).toArray()[0].toObject().value("me"_L1).toBool(), true);
	QCOMPARE(page.json.value("has_more_before"_L1).toBool(true), false);
	QVERIFY(!page.json.contains("has_more_after"_L1));

	// An answer larger than one record (4 x 40 000 characters): reassembled.
	const auto big = peer.call(u"chat.history"_s, {
		{ u"chat"_s, u"big"_s },
		{ u"limit"_s, u"4"_s },
	});
	QVERIFY2(big.ok, qPrintable(big.error.describe()));
	const auto bigMessages = big.json.value("messages"_L1).toArray();
	QCOMPARE(bigMessages.size(), 4);
	for (auto i = 0; i != 4; ++i) {
		QCOMPARE(bigMessages[i].toObject().value("text"_L1).toString(), QString::number(4 - i).repeated(40000));
	}

	const auto state = peer.call(u"sync.state"_s);
	QVERIFY(state.ok);
	QCOMPARE(qint64(state.json.value("pts"_L1).toDouble()), peer.frames[0].pts);

	// Several calls in flight are answered each to its own caller.
	auto answers = QMap<int, int>();
	for (auto limit = 1; limit <= 5; ++limit) {
		peer.client.call(u"chat.history"_s, {
			{ u"chat"_s, u"c1"_s },
			{ u"limit"_s, QString::number(limit) },
		}, QString(), base::Guard(), [&answers, limit](const QJsonValue &value) {
			answers.insert(limit, int(value.toObject().value("messages"_L1).toArray().size()));
		}, nullptr);
	}
	QVERIFY(Test::WaitFor([&] { return answers.size() == 5; }));
	for (auto limit = 1; limit <= 5; ++limit) {
		QCOMPARE(answers.value(limit), limit);
	}
	QVERIFY(peer.client.isConnected());
}

void TestRtInterop::sendAndEcho() {
	auto peer = Peer();
	QVERIFY(peer.connect());
	const auto pts = peer.frames[0].pts;
	const auto id = u"3f0c0a3e-1111-4222-8333-444455556666"_s;
	const auto body = QJsonObject{
		{ u"chat"_s, u"c1"_s },
		{ u"client_message_id"_s, id },
		{ u"kind"_s, u"text"_s },
		{ u"text"_s, u"سلام از دسکتاپ 👋"_s },
		{ u"reply_to_seq"_s, 3 },
		{ u"entities"_s, QJsonArray{
			QJsonObject{ { u"type"_s, u"bold"_s }, { u"offset"_s, 0 }, { u"length"_s, 4 } },
		} },
	};
	const auto sent = peer.call(u"chat.send"_s, body, id);
	QVERIFY2(sent.ok, qPrintable(sent.error.describe()));
	const auto seq = qint64(sent.json.value("seq"_L1).toDouble());
	QVERIFY(seq >= 10);
	QCOMPARE(sent.json.value("message_id"_L1).toString(), u"m_c1_%1"_s.arg(seq));
	QVERIFY(sent.json.value("created_at"_L1).toDouble() > 1.7e12);
	QVERIFY(!sent.json.value("duplicate"_L1).toBool());

	// The request as the other implementation decoded it.
	const auto seen = LastOf(peer.state(), "sends");
	QCOMPARE(seen.value("chat"_L1).toString(), u"c1"_s);
	QCOMPARE(seen.value("client_message_id"_L1).toString(), id);
	QCOMPARE(seen.value("kind"_L1).toString(), u"text"_s);
	QCOMPARE(seen.value("text"_L1).toString(), u"سلام از دسکتاپ 👋"_s);
	QCOMPARE(seen.value("reply_to_seq"_L1).toInt(), 3);
	QCOMPARE(seen.value("idempotency_key"_L1).toString(), id);
	const auto entity = seen.value("entities"_L1).toArray()[0].toObject();
	QCOMPARE(entity.value("type"_L1).toString(), u"bold"_s);
	QCOMPARE(entity.value("length"_L1).toInt(), 4);

	// My own message comes back as a live update.
	QVERIFY(peer.waitFor(Type::Update));
	const auto update = *peer.last(Type::Update);
	QCOMPARE(update.pts, pts + 1);
	QCOMPARE(update.kind, u"message"_s);
	QCOMPARE(update.chat, u"c1"_s);
	QCOMPARE(update.data.value("client_message_id"_L1).toString(), id);
	QCOMPARE(update.data.value("text"_L1).toString(), u"سلام از دسکتاپ 👋"_s);
	QCOMPARE(qint64(update.data.value("seq"_L1).toDouble()), seq);
	QCOMPARE(update.data.value("reply_to_seq"_L1).toInt(), 3);
	QCOMPARE(update.data.value("sender"_L1).toString(), u"u_me"_s);

	// The same client message id again: the server's dedup answers.
	const auto again = peer.call(u"chat.send"_s, body, id);
	QVERIFY(again.ok);
	QVERIFY(again.json.value("duplicate"_L1).toBool());
	QCOMPARE(qint64(again.json.value("seq"_L1).toDouble()), seq);

	// A refusal carries the API's code and status.
	const auto refused = peer.call(u"chat.send"_s, {
		{ u"chat"_s, u"c1"_s },
		{ u"client_message_id"_s, u"another"_s },
		{ u"kind"_s, u"text"_s },
		{ u"text"_s, u"refuse me"_s },
	}, u"another"_s);
	QVERIFY(!refused.ok);
	QCOMPARE(refused.error.code, u"chat.blocked"_s);
	QCOMPARE(refused.error.status, 403);
	QVERIFY(!refused.error.transient());
	QVERIFY(peer.client.isConnected());
}

void TestRtInterop::pushedUpdate() {
	auto peer = Peer();
	QVERIFY(peer.connect());
	const auto pts = peer.frames[0].pts;
	peer.control(Api::Post, u"/control/push"_s, {
		{ u"kind"_s, u"read"_s },
		{ u"data"_s, QJsonObject{ { u"read"_s, QJsonObject{
			{ u"chat"_s, u"c1"_s },
			{ u"reader"_s, u"u_sara"_s },
			{ u"seq"_s, 9 },
		} } } },
	});
	QVERIFY(peer.waitFor(Type::Update));
	auto update = *peer.last(Type::Update);
	QCOMPARE(update.pts, pts + 1);
	QCOMPARE(update.kind, u"read"_s);
	// The oneof is unwrapped: the payload is the kind's own object.
	QCOMPARE(update.data.value("chat"_L1).toString(), u"c1"_s);
	QCOMPARE(update.data.value("reader"_L1).toString(), u"u_sara"_s);
	QCOMPARE(update.data.value("seq"_L1).toInt(), 9);
	QCOMPARE(update.chat, u"c1"_s);

	peer.control(Api::Post, u"/control/push"_s, {
		{ u"kind"_s, u"notif"_s },
		{ u"data"_s, QJsonObject{ { u"notif"_s, QJsonObject{
			{ u"news"_s, 4 },
			{ u"kinds"_s, QJsonObject{ { u"like"_s, 3 }, { u"follow"_s, 1 } } },
		} } } },
	});
	QVERIFY(peer.waitFor(Type::Update, 2));
	update = *peer.last(Type::Update);
	QCOMPARE(update.pts, pts + 2);
	QCOMPARE(update.kind, u"notif"_s);
	QCOMPARE(update.data.value("news"_L1).toInt(), 4);
	QCOMPARE(update.data.value("kinds"_L1).toObject().value("like"_L1).toInt(), 3);

	peer.client.ack(pts + 2);
	QVERIFY(Test::WaitFor([&] {
		const auto acks = peer.state().value("acks"_L1).toArray();
		return !acks.isEmpty() && qint64(acks.last().toDouble()) == pts + 2;
	}));
}

void TestRtInterop::signalsAndSubscriptions() {
	auto peer = Peer();
	QVERIFY(peer.connect());

	QVERIFY(peer.client.sendTyping(u"cap-c1"_s, u"recording_voice"_s));
	QVERIFY(peer.waitFor(Type::Typing));
	const auto typing = *peer.last(Type::Typing);
	QCOMPARE(typing.chat, u"c1"_s);
	QCOMPARE(typing.kind, u"recording_voice"_s);
	QCOMPARE(typing.ttl, 6);
	QCOMPARE(LastSignal(peer.state(), u"typing"_s).value("body"_L1).toObject().value("cap"_L1).toString(), u"cap-c1"_s);
	QVERIFY(!peer.client.sendTyping(QString(), u"typing"_s));

	// The subscription answers with what is known now, then signals follow.
	auto presence = std::optional<QJsonArray>();
	peer.client.subscribePresence({ u"u_sara"_s, u"u_ali"_s }, base::Guard(), [&](const QJsonArray &list) {
		presence = list;
	});
	QVERIFY(Test::WaitFor([&] { return presence.has_value(); }));
	QCOMPARE(presence->size(), 2);
	QCOMPARE((*presence)[0].toObject().value("user"_L1).toString(), u"u_sara"_s);
	QCOMPARE((*presence)[0].toObject().value("status"_L1).toString(), u"recently"_s);
	QCOMPARE(qint64((*presence)[0].toObject().value("last_seen_at"_L1).toDouble()), 1800000000);
	QVERIFY(peer.waitFor(Type::Presence, 2));
	const auto online = *peer.last(Type::Presence);
	QCOMPARE(online.user, u"u_ali"_s);
	QCOMPARE(online.status, u"online"_s);
	QVERIFY(!online.reset);

	peer.client.followProfiles({ u"u_sara"_s });
	peer.client.subscribeTopics(u"tech"_s);
	peer.client.subscribeFeed(1800000000123);
	peer.client.sendLiveLocation(u"c1"_s, 12, { { u"lat"_s, 35.7 }, { u"lng"_s, 51.4 } });
	peer.client.setBackground(true);
	QVERIFY(Test::WaitFor([&] {
		return !LastSignal(peer.state(), u"state"_s).isEmpty();
	}));
	const auto state = peer.state();
	QCOMPARE(LastSignal(state, u"profile_sub"_s).value("body"_L1).toObject().value("users"_L1).toArray().size(), 1);
	QCOMPARE(LastSignal(state, u"topics_sub"_s).value("body"_L1).toObject().value("category"_L1).toString(), u"tech"_s);
	QCOMPARE(qint64(LastSignal(state, u"feed_sub"_s).value("body"_L1).toObject().value("since"_L1).toDouble()), 1800000000123);
	const auto live = LastSignal(state, u"live_location"_s).value("body"_L1).toObject();
	QCOMPARE(live.value("chat"_L1).toString(), u"c1"_s);
	QCOMPARE(live.value("seq"_L1).toInt(), 12);
	QCOMPARE(live.value("location"_L1).toObject().value("lat"_L1).toDouble(), 35.7);
	QCOMPARE(LastSignal(state, u"state"_s).value("body"_L1).toObject().value("background"_L1).toBool(), true);

	// Signals the server sends on its own.
	peer.control(Api::Post, u"/control/signal"_s, {
		{ u"t"_s, u"feed_new"_s },
		{ u"message"_s, u"FeedNewSignal"_s },
		{ u"body"_s, QJsonObject{ { u"count"_s, 3 }, { u"since"_s, 1800000000123. } } },
	});
	peer.control(Api::Post, u"/control/signal"_s, {
		{ u"t"_s, u"profile"_s },
		{ u"message"_s, u"ProfileSignal"_s },
		{ u"body"_s, QJsonObject{ { u"user"_s, u"u_sara"_s }, { u"v"_s, 7 } } },
	});
	peer.control(Api::Post, u"/control/signal"_s, {
		{ u"t"_s, u"topics"_s },
		{ u"message"_s, u"TopicsSignal"_s },
		{ u"body"_s, QJsonObject{ { u"changed"_s, true }, { u"category"_s, u"tech"_s } } },
	});
	peer.control(Api::Post, u"/control/signal"_s, { { u"t"_s, u"resync"_s } });
	QVERIFY(peer.waitFor(Type::Resync));
	QCOMPARE(peer.last(Type::FeedNew)->count, 3);
	QCOMPARE(peer.last(Type::FeedNew)->since, 1800000000123);
	QCOMPARE(peer.last(Type::Profile)->user, u"u_sara"_s);
	QCOMPARE(peer.last(Type::Topics)->category, u"tech"_s);
}

void TestRtInterop::reconnectResumes() {
	auto peer = Peer();
	auto local = qint64(0);
	peer.client.setPtsProvider([&] { return local; });
	QVERIFY(peer.connect());
	local = peer.frames[0].pts;
	peer.client.subscribePresence({ u"u_sara"_s });
	peer.client.subscribeTopics(QString());
	QVERIFY(peer.waitFor(Type::Presence));

	// Three updates happen while the connection is being replaced.
	const auto signalsBefore = peer.state().value("signals"_L1).toArray().size();
	peer.control(Api::Post, u"/control/pts"_s, { { u"pts"_s, double(local + 3) } });
	peer.control(Api::Post, u"/control/close"_s, { { u"code"_s, 4000 } });
	QVERIFY(peer.waitFor(Type::Disconnected));
	QVERIFY(peer.waitFor(Type::Resumed));

	// Disconnected, Hello(resumed), the replay in order, Resumed.
	auto order = QVector<Type>();
	auto replayed = QVector<Rt::Frame>();
	auto begin = false;
	for (const auto &frame : peer.frames) {
		begin = begin || (frame.type == Type::Disconnected);
		if (begin && frame.type != Type::Presence) {
			order.push_back(frame.type);
		}
		if (frame.type == Type::Replayed) {
			replayed.push_back(frame);
		}
	}
	QCOMPARE(order, (QVector<Type>{
		Type::Disconnected,
		Type::Hello,
		Type::Replayed,
		Type::Replayed,
		Type::Replayed,
		Type::Resumed,
	}));
	const auto hello = *peer.last(Type::Hello);
	QVERIFY(hello.resumed);
	QCOMPARE(hello.pts, local + 3);
	for (auto i = 0; i != 3; ++i) {
		const auto pts = local + 1 + i;
		QCOMPARE(replayed[i].pts, pts);
		QCOMPARE(replayed[i].kind, u"message"_s);
		QCOMPARE(replayed[i].chat, u"c1"_s);
		// A difference item: the stored view rides in `message`.
		QCOMPARE(qint64(replayed[i].data.value("seq"_L1).toDouble()), 1000 + pts);
		QCOMPARE(
			replayed[i].data.value("message"_L1).toObject().value("text"_L1).toString(),
			u"replayed %1"_s.arg(pts));
	}
	// The new connection told the server where we were ...
	const auto state = peer.state();
	QCOMPARE(qint64(LastOf(state, "auths").value("pts"_L1).toDouble()), local);
	// ... and the subscriptions were made again without being asked.
	auto resent = QStringList();
	const auto all = state.value("signals"_L1).toArray();
	for (auto i = signalsBefore; i < all.size(); ++i) {
		resent.push_back(all[i].toObject().value("t"_L1).toString());
	}
	QVERIFY2(resent.contains(u"presence_sub"_s), qPrintable(resent.join(u',')));
	QVERIFY2(resent.contains(u"topics_sub"_s), qPrintable(resent.join(u',')));
}

void TestRtInterop::reauthClose() {
	auto peer = Peer();
	QVERIFY(peer.connect());
	// 4002: the token must be refreshed before the next attempt.
	peer.delegate.tokenAfterRefresh = u"tok-2"_s;
	peer.control(Api::Post, u"/control/close"_s, { { u"code"_s, 4002 } });
	QVERIFY(peer.waitFor(Type::Hello, 2));
	QCOMPARE(peer.delegate.refreshes, 1);
	QCOMPARE(LastOf(peer.state(), "auths").value("token"_L1).toString(), u"tok-2"_s);
	QVERIFY(peer.client.isConnected());
	QCOMPARE(peer.delegate.revoked, 0);
}

void TestRtInterop::expiredTokenProof() {
	auto peer = Peer();
	peer.control(Api::Post, u"/control/device"_s, { { u"pub"_s, peer.delegate.key.publicKeyBase64() } });
	// An expired access token inside the resume grace: the server asks the
	// device to sign its nonce; a valid signature signs in without a refresh.
	peer.delegate.token = u"expired-1"_s;
	QVERIFY(peer.connect());
	QCOMPARE(peer.delegate.proofs, 1);
	QCOMPARE(peer.delegate.refreshes, 0);
	auto state = peer.state();
	QCOMPARE(state.value("proofs"_L1).toArray().last().toBool(), true);
	QVERIFY(peer.frames[0].type == Type::Hello);
	// The connection is a normal one from here on.
	QVERIFY(peer.call(u"me.get"_s).ok);
	peer.client.stop();

	// A signature by another key is refused; the client falls back to a token refresh.
	auto other = Peer();
	other.delegate.token = u"expired-2"_s;
	other.delegate.tokenAfterRefresh = u"tok-fresh"_s;
	QVERIFY(other.connect());
	state = other.state();
	QCOMPARE(state.value("proofs"_L1).toArray().last().toBool(), false);
	QCOMPARE(other.delegate.proofs, 1);
	QCOMPARE(other.delegate.refreshes, 1);
	QCOMPARE(LastOf(state, "auths").value("token"_L1).toString(), u"tok-fresh"_s);
}

void TestRtInterop::keyRotation() {
	auto peer = Peer();
	QVERIFY(peer.connect());
	const auto before = peer.state().value("key_id"_L1).toInt();

	// Rotated while connected: Close 4006, new keys, a new handshake.
	const auto rotated = peer.control(Api::Post, u"/control/rotate"_s).value("id"_L1).toInt();
	QCOMPARE(rotated, before + 1);
	QVERIFY(peer.waitFor(Type::Hello, 2));
	QCOMPARE(LastOf(peer.state(), "auths").value("key_id"_L1).toInt(), rotated);
	QVERIFY(peer.call(u"me.get"_s).ok);

	// Rotated while we were away: the cached key gets the clear-text close,
	// the listing is read again and the next attempt succeeds.
	peer.client.stop();
	QVERIFY(Test::WaitFor([&] { return peer.state().value("connections"_L1).toInt() == 0; }));
	const auto next = peer.control(Api::Post, u"/control/rotate"_s).value("id"_L1).toInt();
	const auto hellos = peer.count(Type::Hello);
	QVERIFY(peer.connect());
	QCOMPARE(peer.count(Type::Hello), hellos + 1);
	const auto state = peer.state();
	const auto rejected = state.value("rejectedKeyIds"_L1).toArray();
	QVERIFY(!rejected.isEmpty());
	QCOMPARE(rejected.last().toInt(), rotated);
	QCOMPARE(LastOf(state, "auths").value("key_id"_L1).toInt(), next);
	QVERIFY(peer.call(u"me.get"_s).ok);
}

void TestRtInterop::revoked() {
	auto peer = Peer();
	peer.delegate.token = u"revoked-1"_s;
	peer.client.start();
	QVERIFY(Test::WaitFor([&] { return peer.delegate.revoked == 1; }, 15000));
	QVERIFY(peer.client.state() == Rt::State::Stopped);
	QVERIFY(!peer.client.isConnected());
	QCOMPARE(peer.count(Type::Hello), 0);
	// It does not come back by itself.
	Test::Settle(700);
	QCOMPARE(peer.delegate.revoked, 1);
	QVERIFY(peer.client.state() == Rt::State::Stopped);
}

// Starts the peer; 77 when it cannot run here.
int main(int argc, char *argv[]) {
	auto app = QCoreApplication(argc, argv);
	const auto env = QProcessEnvironment::systemEnvironment();
	auto node = env.value(u"ELEZA_NODE"_s);
	if (node.isEmpty() || !QFile::exists(node)) {
		node = QStandardPaths::findExecutable(u"node"_s);
	}
	const auto sources = env.value(u"ELEZA_RTPEER_DIR"_s, QStringLiteral(ELEZA_TESTDATA_DIR "/../rtpeer"));
	if (node.isEmpty() || !QFile::exists(sources + u"/run.mjs"_s)) {
		qWarning("rt interop skipped: node or tests/core/rtpeer not found");
		return 77;
	}
	auto process = QProcess();
	process.setProcessChannelMode(QProcess::SeparateChannels);
	process.setReadChannel(QProcess::StandardOutput);
	process.start(node, { sources + u"/run.mjs"_s });
	// The first run installs the peer's dependencies: allow for that.
	auto timer = QElapsedTimer();
	timer.start();
	auto line = QByteArray();
	while (timer.elapsed() < 240000 && !line.startsWith("READY ")) {
		if (process.state() == QProcess::NotRunning && !process.canReadLine()) {
			break;
		}
		process.waitForReadyRead(200);
		if (process.canReadLine()) {
			line = process.readLine().trimmed();
		}
	}
	if (!line.startsWith("READY ")) {
		const auto errors = process.readAllStandardError().trimmed();
		const auto skipped = (process.error() == QProcess::FailedToStart)
			|| ((process.state() == QProcess::NotRunning) && (process.exitCode() == 77));
		process.kill();
		process.waitForFinished(3000);
		qWarning("rt interop %s: %s", skipped ? "skipped" : "could not start the peer", errors.constData());
		return skipped ? 77 : 1;
	}
	PeerBase = u"http://127.0.0.1:"_s + QString::fromLatin1(line.mid(6));
	QObject::connect(&process, &QProcess::readyReadStandardError, &process, [&] {
		fputs(process.readAllStandardError().constData(), stderr);
	});

	auto test = TestRtInterop();
	const auto result = QTest::qExec(&test, argc, argv);
	process.closeWriteChannel(); // the peer exits when its stdin ends
	if (!process.waitForFinished(3000)) {
		process.kill();
		process.waitForFinished(3000);
	}
	return result;
}

#include "test_rt_interop.moc"
