// Data::Sync on a fake transport: which frames are applied, when the
// difference is read, what is acknowledged, and that nothing polls while the
// connection is up.
#include "api/api_client_config.h"
#include "data/data_graph.h"
#include "data/data_json.h"
#include "data/data_notifications.h"
#include "test_harness.h"

#include <QtCore/QTemporaryDir>
#include <QtTest/QSignalSpy>

using namespace Data;
using Test::Harness;
using Reply = Test::FakeTransport::Reply;
using Call = Test::FakeTransport::Call;
using Type = Rt::Frame::Type;

namespace {

[[nodiscard]] Rt::Frame Hello(qint64 pts, bool resumed = false) {
	auto frame = Rt::Frame();
	frame.type = Type::Hello;
	frame.pts = pts;
	frame.resumed = resumed;
	return frame;
}

[[nodiscard]] Rt::Frame Signal(Type type) {
	auto frame = Rt::Frame();
	frame.type = type;
	return frame;
}

[[nodiscard]] Rt::Frame Update(qint64 pts, const QString &kind = QString(), const QJsonObject &data = {}) {
	auto frame = Rt::Frame();
	frame.type = Type::Update;
	frame.pts = pts;
	frame.kind = kind;
	frame.data = data;
	return frame;
}

[[nodiscard]] QJsonObject MessageJson(
		const QString &chat,
		qint64 seq,
		const QString &sender,
		const QString &text) {
	return QJsonObject{
		{ u"chat"_s, chat },
		{ u"seq"_s, double(seq) },
		{ u"id"_s, u"m_%1_%2"_s.arg(chat).arg(seq) },
		{ u"sender"_s, sender },
		{ u"kind"_s, u"text"_s },
		{ u"text"_s, text },
		{ u"created_at"_s, 1800000000000. + double(seq) },
	};
}

// A difference item as GET /v1/sync/difference and the replay carry it.
[[nodiscard]] QJsonObject Item(qint64 pts, const QJsonObject &message) {
	return QJsonObject{
		{ u"pts"_s, QString::number(pts) },
		{ u"kind"_s, u"message"_s },
		{ u"chat"_s, message.value("chat"_L1) },
		{ u"seq"_s, message.value("seq"_L1) },
		{ u"message"_s, message },
	};
}

[[nodiscard]] Rt::Frame Replayed(qint64 pts, const QJsonObject &message) {
	auto frame = Rt::Frame();
	frame.type = Type::Replayed;
	frame.pts = pts;
	frame.kind = u"message"_s;
	frame.data = Item(pts, message);
	return frame;
}

[[nodiscard]] QJsonObject Difference(qint64 pts, const QJsonArray &updates) {
	return QJsonObject{ { u"pts"_s, QString::number(pts) }, { u"updates"_s, updates } };
}

[[nodiscard]] QStringList DifferenceFrom(const Harness &harness) {
	auto result = QStringList();
	for (const auto &body : harness.bodies(u"sync.difference"_s)) {
		result.push_back(body.value("pts"_L1).toString());
	}
	return result;
}

// Starts, loads the timeline of c1 and brings the connection up at `pts`.
[[nodiscard]] bool Connect(Harness &harness, qint64 pts) {
	if (!harness.start()) {
		return false;
	}
	auto done = false;
	harness.messages().loadLatest(u"c1"_s, base::Guard(), [&] { done = true; });
	if (!Test::WaitFor([&] { return done; })) {
		return false;
	}
	harness.transport.setOnline(true);
	harness.transport.emitFrame(Hello(pts));
	return true;
}


// A page of c1's history: the messages `from`..`to` (newest first, as the
// server sends them) under the header of c1 at `lastSeq`.
[[nodiscard]] QJsonObject HistoryPage(
		qint64 from,
		qint64 to,
		bool moreBefore,
		qint64 lastSeq,
		std::optional<bool> moreAfter = std::nullopt) {
	auto header = Test::Fixture("history_c1.json").value("chat"_L1).toObject();
	header.insert(u"last_seq"_s, double(lastSeq));
	auto messages = QJsonArray();
	for (auto seq = to; seq >= from; --seq) {
		messages.push_back(MessageJson(u"c1"_s, seq, u"u_sara"_s, QString::number(seq)));
	}
	auto result = QJsonObject{
		{ u"chat"_s, header },
		{ u"messages"_s, messages },
		{ u"has_more_before"_s, moreBefore },
	};
	if (moreAfter) {
		result.insert(u"has_more_after"_s, *moreAfter);
	}
	return result;
}

[[nodiscard]] qint64 Param(const Call &call, const char *name) {
	return call.body.value(QLatin1StringView(name)).toString().toLongLong();
}

[[nodiscard]] QVector<qint64> Seqs(qint64 from, qint64 to) {
	auto result = QVector<qint64>();
	for (auto seq = from; seq <= to; ++seq) {
		result.push_back(seq);
	}
	return result;
}

// The server messages of a chat's timeline, oldest first.
[[nodiscard]] QVector<qint64> HeldSeqs(const Harness &harness, const QString &chat) {
	auto result = QVector<qint64>();
	for (const auto &message : harness.messages().history(chat).items) {
		if (message.seq > 0) {
			result.push_back(message.seq);
		}
	}
	return result;
}

// What the database holds of a chat, oldest first.
[[nodiscard]] QVector<qint64> CachedSeqs(const Harness &harness, const QString &chat) {
	auto result = QVector<qint64>();
	auto done = false;
	harness.database->messages(chat, 1000, 0, base::Guard(), [&](QVector<QJsonObject> rows) {
		for (const auto &row : rows) {
			result.push_back(Message::FromStored(row).seq);
		}
		done = true;
	});
	const auto read = Test::WaitFor([&] { return done; });
	Q_ASSERT(read);
	Q_UNUSED(read);
	return result;
}

// The database's copy of one message.
[[nodiscard]] std::optional<Message> CachedMessage(
		const Harness &harness,
		const QString &chat,
		qint64 seq) {
	auto result = std::optional<Message>();
	auto done = false;
	harness.database->messages(chat, 1, seq + 1, base::Guard(), [&](QVector<QJsonObject> rows) {
		if (!rows.isEmpty()) {
			auto message = Message::FromStored(rows.front());
			if (message.seq == seq) {
				result = std::move(message);
			}
		}
		done = true;
	});
	const auto read = Test::WaitFor([&] { return done; });
	Q_ASSERT(read);
	Q_UNUSED(read);
	return result;
}

// Opens c1 as the chat screen does: the cache first, then the newest page.
[[nodiscard]] bool OpenC1(Harness &harness) {
	auto cached = false;
	harness.messages().loadCached(u"c1"_s, base::Guard(), [&](int) { cached = true; });
	if (!Test::WaitFor([&] { return cached; })) {
		return false;
	}
	auto loaded = false;
	harness.messages().loadLatest(u"c1"_s, base::Guard(), [&] { loaded = true; });
	return Test::WaitFor([&] { return loaded; });
}

// A first run that reads c1 (1..9, the fixture) and caches it.
[[nodiscard]] bool CacheC1(const QString &path) {
	auto harness = Harness(path);
	return Connect(harness, 100) && (CachedSeqs(harness, u"c1"_s) == Seqs(1, 9));
}

} // namespace

class TestSync final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void firstHelloAdoptsServerPts();
	void liveUpdatesApplyInOrder();
	void gapReadsTheDifference();
	void differenceIsPaged();
	void tooLongStartsOver();
	void differenceKeepsTheHighestPts();
	void differenceGoesOnAfterThePageLimit();
	void settingsAndConfigSignals();
	void resumedReplay();
	void holeInReplay();
	void helloAheadWithoutReplay();
	void resyncSignal();
	void acksEverySixteen();
	void updateKinds();
	void messageForUnloadedTimeline();
	void transientFrames();
	void ptsSurvivesRestart();
	void fallbackOnlyWhileDisconnected();
	void offlineStartReadsStateAtOnce();
	void newestPageAttachesToTheCache();
	void windowReachingTheEndIsCached();
	void changesOutsideTheWindowReachTheCache();
	void differenceItemsWithoutPayload();

};

void TestSync::firstHelloAdoptsServerPts() {
	auto harness = Harness();
	// The socket is up before the session is ready: nothing is asked.
	harness.transport.setOnline(true);
	QVERIFY(harness.start());
	QVERIFY(!harness.sync().pts());
	QVERIFY(harness.transport.ptsProvider);
	QCOMPARE(harness.transport.ptsProvider(), 0);
	auto changed = QSignalSpy(&harness.sync(), &Sync::ptsChanged);
	harness.transport.setOnline(true);
	harness.transport.emitFrame(Hello(100));
	QCOMPARE(harness.sync().pts().value_or(-1), 100);
	QCOMPARE(harness.transport.ptsProvider(), 100);
	QCOMPARE(changed.size(), 1);
	QVERIFY(!harness.sync().updating());
	Test::Settle();
	// Nothing to catch up with: no state, no difference.
	QCOMPARE(harness.transport.count(u"sync.difference"_s), 0);
	QCOMPARE(harness.transport.count(u"sync.state"_s), 0);
}

void TestSync::liveUpdatesApplyInOrder() {
	auto harness = Harness();
	QVERIFY(Connect(harness, 100));
	auto received = QSignalSpy(&harness.sync(), &Sync::messageReceived);
	const auto unread = harness.chats().chat(u"c1"_s)->unread;
	const auto count = harness.messages().history(u"c1"_s).items.size();

	harness.transport.emitFrame(Update(101, u"message"_s, MessageJson(u"c1"_s, 10, u"u_sara"_s, u"نو"_s)));
	QCOMPARE(harness.sync().pts().value_or(-1), 101);
	QCOMPARE(harness.messages().history(u"c1"_s).items.size(), count + 1);
	const auto message = harness.messages().find(u"c1"_s, 10);
	QVERIFY(message);
	QCOMPARE(message->text, u"نو"_s);
	QVERIFY(!message->mine);
	auto row = harness.chats().chat(u"c1"_s);
	QCOMPARE(row->lastSeq, 10);
	QCOMPARE(row->unread, unread + 1);
	QCOMPARE(row->lastMessage->seq, 10);
	QVERIFY(!row->lastMine);
	QCOMPARE(received.size(), 1);
	QCOMPARE(received[0][0].value<Data::Message>().seq, 10);

	// The same update again (a retransmit): nothing changes.
	harness.transport.emitFrame(Update(101, u"message"_s, MessageJson(u"c1"_s, 10, u"u_sara"_s, u"نو"_s)));
	QCOMPARE(harness.chats().chat(u"c1"_s)->unread, unread + 1);
	QCOMPARE(received.size(), 1);

	// My own message from another device: no unread, no notification.
	harness.transport.emitFrame(Update(102, u"message"_s, MessageJson(u"c1"_s, 11, u"u_me"_s, u"از گوشی"_s)));
	row = harness.chats().chat(u"c1"_s);
	QCOMPARE(row->unread, unread + 1);
	QVERIFY(row->lastMine);
	QVERIFY(harness.messages().find(u"c1"_s, 11)->mine);
	QCOMPARE(received.size(), 1);

	// The chat on screen does not count unread.
	harness.chats().setVisibleChat(u"c1"_s);
	harness.transport.emitFrame(Update(103, u"message"_s, MessageJson(u"c1"_s, 12, u"u_sara"_s, u"دیدی؟"_s)));
	QCOMPARE(harness.chats().chat(u"c1"_s)->unread, unread + 1);
	QCOMPARE(received.size(), 2);
	QCOMPARE(harness.sync().pts().value_or(-1), 103);
	Test::Settle();
	QCOMPARE(harness.transport.count(u"sync.difference"_s), 0);
}

void TestSync::gapReadsTheDifference() {
	auto harness = Harness();
	QVERIFY(Connect(harness, 100));
	harness.on(u"sync.difference"_s, [](const Call &) {
		return Reply::Ok(Difference(103, QJsonArray{
			Item(101, MessageJson(u"c1"_s, 10, u"u_sara"_s, u"a"_s)),
			Item(102, MessageJson(u"c1"_s, 11, u"u_sara"_s, u"b"_s)),
			Item(103, MessageJson(u"c1"_s, 12, u"u_sara"_s, u"c"_s)),
		}));
	});
	auto updating = QSignalSpy(&harness.sync(), &Sync::updatingChanged);
	auto received = QSignalSpy(&harness.sync(), &Sync::messageReceived);

	// 101 and 102 never arrived.
	harness.transport.emitFrame(Update(103, u"message"_s, MessageJson(u"c1"_s, 12, u"u_sara"_s, u"c"_s)));
	QCOMPARE(harness.sync().pts().value_or(-1), 100);
	QVERIFY(harness.sync().updating());
	QVERIFY(harness.transport.updating);
	QVERIFY(harness.messages().find(u"c1"_s, 12) == nullptr);
	// More frames during the difference do not start a second one.
	harness.transport.emitFrame(Update(104));
	QVERIFY(Test::WaitFor([&] { return harness.sync().pts().value_or(0) >= 103 && !harness.sync().updating(); }));

	// The frame that came during the difference asked for one more round.
	QCOMPARE(DifferenceFrom(harness), (QStringList{ u"100"_s, u"103"_s }));
	QCOMPARE(harness.messages().find(u"c1"_s, 10)->text, u"a"_s);
	QCOMPARE(harness.messages().find(u"c1"_s, 11)->text, u"b"_s);
	QCOMPARE(harness.messages().find(u"c1"_s, 12)->text, u"c"_s);
	QCOMPARE(harness.chats().chat(u"c1"_s)->lastSeq, 12);
	QVERIFY(!harness.transport.updating);
	QCOMPARE(updating.size(), 2);
	QCOMPARE(updating[0][0].toBool(), true);
	QCOMPARE(updating[1][0].toBool(), false);
	// Caught-up messages are not announced as live ones.
	QCOMPARE(received.size(), 0);
}

void TestSync::differenceIsPaged() {
	auto harness = Harness();
	QVERIFY(Connect(harness, 100));
	harness.on(u"sync.difference"_s, [](const Call &call) {
		const auto from = call.body.value("pts"_L1).toString().toLongLong();
		auto updates = QJsonArray();
		for (auto pts = from + 1; pts <= std::min<qint64>(from + 2, 105); ++pts) {
			updates.push_back(Item(pts, MessageJson(u"c1"_s, pts - 91, u"u_sara"_s, QString::number(pts))));
		}
		return Reply::Ok(Difference(105, updates));
	});
	harness.sync().catchUp();
	QVERIFY(Test::WaitFor([&] { return !harness.sync().updating(); }));
	QCOMPARE(harness.sync().pts().value_or(-1), 105);
	QCOMPARE(DifferenceFrom(harness), (QStringList{ u"100"_s, u"102"_s, u"104"_s }));
	QCOMPARE(harness.messages().find(u"c1"_s, 14)->text, u"105"_s);
	QCOMPARE(harness.messages().history(u"c1"_s).lastSeq(), 14);
}

void TestSync::tooLongStartsOver() {
	auto harness = Harness();
	QVERIFY(Connect(harness, 100));
	harness.on(u"sync.difference"_s, [](const Call &call) {
		return (call.body.value("pts"_L1).toString() == u"100")
			? Reply::Ok(QJsonObject{ { u"too_long"_s, true }, { u"pts"_s, u"500"_s } })
			: Reply::Ok(Difference(520, QJsonArray()));
	});
	harness.on(u"sync.state"_s, [](const Call &) {
		return Reply::Ok(QJsonObject{ { u"pts"_s, u"520"_s } });
	});
	const auto inboxLoads = harness.transport.count(u"chat.inbox"_s);
	// (One state read happened at the start, before the connection was up.)
	const auto stateReads = harness.transport.count(u"sync.state"_s);
	QVERIFY(harness.messages().history(u"c1"_s).loaded);
	harness.transport.emitFrame(Signal(Type::Resync));
	QVERIFY(Test::WaitFor([&] { return !harness.sync().updating(); }));
	Test::Settle();
	QCOMPARE(harness.sync().pts().value_or(-1), 520);
	QCOMPARE(harness.transport.count(u"sync.state"_s), stateReads + 1);
	QCOMPARE(DifferenceFrom(harness), (QStringList{ u"100"_s, u"520"_s }));
	// Everything cached is read again: the inbox now, timelines when opened.
	QVERIFY(harness.transport.count(u"chat.inbox"_s) > inboxLoads);
	QVERIFY(!harness.messages().history(u"c1"_s).loaded);
}

void TestSync::differenceKeepsTheHighestPts() {
	auto harness = Harness();
	QVERIFY(Connect(harness, 100));
	harness.on(u"sync.difference"_s, [](const Call &call) {
		if (call.body.value("pts"_L1).toString() != u"100") {
			return Reply::Ok(Difference(104, QJsonArray()));
		}
		// The last item carries no pts: the point must not go back to 0
		// (which would read the whole log - too_long - next).
		auto bare = Item(0, MessageJson(u"c1"_s, 12, u"u_sara"_s, u"c"_s));
		bare.remove(u"pts"_s);
		return Reply::Ok(Difference(104, QJsonArray{
			Item(101, MessageJson(u"c1"_s, 10, u"u_sara"_s, u"a"_s)),
			Item(102, MessageJson(u"c1"_s, 11, u"u_sara"_s, u"b"_s)),
			bare,
		}));
	});
	const auto states = harness.transport.count(u"sync.state"_s);
	harness.sync().catchUp();
	QVERIFY(Test::WaitFor([&] { return !harness.sync().updating(); }));
	QCOMPARE(DifferenceFrom(harness), (QStringList{ u"100"_s, u"102"_s }));
	QCOMPARE(harness.sync().pts().value_or(-1), 104);
	QVERIFY(harness.messages().find(u"c1"_s, 12) != nullptr);
	QCOMPARE(harness.transport.count(u"sync.state"_s), states);
}

void TestSync::differenceGoesOnAfterThePageLimit() {
	auto harness = Harness();
	QVERIFY(Connect(harness, 100));
	// One update per page, more pages than one round reads.
	const auto end = qint64(100 + Sync::kMaxDifferencePages + 5);
	harness.on(u"sync.difference"_s, [=](const Call &call) {
		const auto from = call.body.value("pts"_L1).toString().toLongLong();
		auto updates = QJsonArray();
		if (from < end) {
			updates.push_back(QJsonObject{
				{ u"pts"_s, QString::number(from + 1) },
				{ u"kind"_s, u"notif"_s },
				{ u"data"_s, QJsonObject{ { u"news"_s, double(from + 1 - 100) } } },
			});
		}
		return Reply::Ok(Difference(end, updates));
	});
	harness.sync().catchUp();
	QVERIFY(Test::WaitFor([&] { return !harness.sync().updating(); }, 15000));
	QCOMPARE(harness.sync().pts().value_or(-1), end);
	QCOMPARE(harness.transport.count(u"sync.difference"_s), int(end - 100));
}

void TestSync::settingsAndConfigSignals() {
	auto settingsReads = 0;
	auto configReads = 0;
	auto configServer = Test::HttpServer();
	configServer.handler = [&](const Test::HttpServer::Request &) {
		++configReads;
		return Test::HttpServer::Response::Ok(QJsonObject{
			{ u"version"_s, 7 },
			{ u"values"_s, QJsonObject() },
		});
	};
	auto configApi = Api::Client(configServer.base(), u"1.0.0"_s);
	auto config = Api::ClientConfig(&configApi, nullptr);
	auto loaded = false;
	config.load(nullptr, [&](bool) { loaded = true; });
	QVERIFY(Test::WaitFor([&] { return loaded; }));
	QCOMPARE(config.version(), 7);

	auto options = Harness::Options();
	options.clientConfig = &config;
	auto harness = Harness(options);
	harness.http.handler = [&](const Test::HttpServer::Request &request) {
		if (request.path == "/v1/me/settings") {
			++settingsReads;
			return Test::HttpServer::Response::Ok(QJsonObject{
				{ u"privacy"_s, QJsonObject{ { u"read_receipts"_s, settingsReads < 2 } } },
			});
		}
		return Test::HttpServer::Response::Ok();
	};
	QVERIFY(harness.start());
	auto &graph = harness.session->graph();
	const auto signal = [&](Type type, qint64 version) {
		auto frame = Rt::Frame();
		frame.type = type;
		frame.version = version;
		harness.transport.emitFrame(frame);
	};

	// Settings nobody read yet are not read because of the signal.
	signal(Type::Settings, 3);
	Test::Settle(50);
	QCOMPARE(settingsReads, 0);
	auto read = false;
	graph.loadSettings(false, base::Guard(), [&](const QJsonObject &) { read = true; });
	QVERIFY(Test::WaitFor([&] { return read; }));
	QCOMPARE(settingsReads, 1);
	QVERIFY(graph.settingValue(u"privacy.read_receipts"_s).toBool());
	// Changed on another device: read again, once per version.
	auto changed = QSignalSpy(&graph, &Graph::settingsChanged);
	signal(Type::Settings, 4);
	QVERIFY(Test::WaitFor([&] { return settingsReads == 2 && !changed.isEmpty(); }));
	QVERIFY(!graph.settingValue(u"privacy.read_receipts"_s).toBool());
	signal(Type::Settings, 4);
	Test::Settle(50);
	QCOMPARE(settingsReads, 2);

	// client-config: the version held is not read again, a new one is.
	const auto reads = configReads;
	signal(Type::Config, 7);
	Test::Settle(50);
	QCOMPARE(configReads, reads);
	signal(Type::Config, 8);
	QVERIFY(Test::WaitFor([&] { return configReads == reads + 1; }));
}

void TestSync::resumedReplay() {
	auto harness = Harness();
	QVERIFY(Connect(harness, 100));
	harness.transport.setOnline(false);
	harness.transport.emitFrame(Signal(Type::Disconnected));
	auto updating = QSignalSpy(&harness.sync(), &Sync::updatingChanged);
	auto received = QSignalSpy(&harness.sync(), &Sync::messageReceived);
	harness.transport.acks.clear();

	harness.transport.setOnline(true);
	harness.transport.emitFrame(Hello(103, true));
	QVERIFY(harness.sync().updating());
	QCOMPARE(harness.sync().pts().value_or(-1), 100);
	harness.transport.emitFrame(Replayed(101, MessageJson(u"c1"_s, 10, u"u_sara"_s, u"a"_s)));
	harness.transport.emitFrame(Replayed(101, MessageJson(u"c1"_s, 10, u"u_sara"_s, u"a"_s))); // twice
	harness.transport.emitFrame(Replayed(102, MessageJson(u"c1"_s, 11, u"u_sara"_s, u"b"_s)));
	harness.transport.emitFrame(Replayed(103, MessageJson(u"c2"_s, 5, u"u_ali"_s, u"c"_s)));
	QVERIFY(harness.sync().updating());
	QVERIFY(harness.transport.acks.isEmpty());
	harness.transport.emitFrame(Signal(Type::Resumed));

	QVERIFY(!harness.sync().updating());
	QCOMPARE(harness.sync().pts().value_or(-1), 103);
	QCOMPARE(harness.messages().find(u"c1"_s, 11)->text, u"b"_s);
	QCOMPARE(harness.chats().chat(u"c2"_s)->lastSeq, 5);
	QCOMPARE(harness.chats().chat(u"c2"_s)->lastMessage->text, u"c"_s);
	// The replay is acknowledged once it is complete.
	QCOMPARE(harness.transport.acks, QVector<qint64>{ 103 });
	QCOMPARE(updating.size(), 2);
	QCOMPARE(received.size(), 0);
	Test::Settle();
	// The connection brought everything: the difference was not needed.
	QCOMPARE(harness.transport.count(u"sync.difference"_s), 0);
}

void TestSync::holeInReplay() {
	auto harness = Harness();
	QVERIFY(Connect(harness, 100));
	harness.on(u"sync.difference"_s, [](const Call &) {
		return Reply::Ok(Difference(103, QJsonArray{
			Item(102, MessageJson(u"c1"_s, 11, u"u_sara"_s, u"b"_s)),
			Item(103, MessageJson(u"c1"_s, 12, u"u_sara"_s, u"c"_s)),
		}));
	});
	harness.transport.emitFrame(Hello(103, true));
	harness.transport.emitFrame(Replayed(101, MessageJson(u"c1"_s, 10, u"u_sara"_s, u"a"_s)));
	harness.transport.emitFrame(Replayed(103, MessageJson(u"c1"_s, 12, u"u_sara"_s, u"c"_s)));
	// 103 cannot be applied before 102.
	QCOMPARE(harness.sync().pts().value_or(-1), 101);
	QVERIFY(harness.messages().find(u"c1"_s, 12) == nullptr);
	Test::Settle();
	QCOMPARE(harness.transport.count(u"sync.difference"_s), 0); // not while replaying
	harness.transport.emitFrame(Signal(Type::Resumed));
	QVERIFY(Test::WaitFor([&] { return harness.sync().pts().value_or(0) == 103 && !harness.sync().updating(); }));
	QCOMPARE(DifferenceFrom(harness), QStringList{ u"101"_s });
	QCOMPARE(harness.messages().find(u"c1"_s, 11)->text, u"b"_s);
	QCOMPARE(harness.messages().find(u"c1"_s, 12)->text, u"c"_s);
}

void TestSync::helloAheadWithoutReplay() {
	auto harness = Harness();
	QVERIFY(Connect(harness, 100));
	harness.on(u"sync.difference"_s, [](const Call &) {
		return Reply::Ok(Difference(101, QJsonArray{
			Item(101, MessageJson(u"c1"_s, 10, u"u_sara"_s, u"a"_s)),
		}));
	});
	// The server could not resume (its queue was gone) and is ahead of us.
	harness.transport.emitFrame(Hello(101, false));
	QVERIFY(Test::WaitFor([&] { return harness.sync().pts().value_or(0) == 101 && !harness.sync().updating(); }));
	QCOMPARE(DifferenceFrom(harness), QStringList{ u"100"_s });
	// A Hello at our pts needs nothing.
	harness.transport.emitFrame(Hello(101, false));
	Test::Settle();
	QCOMPARE(harness.transport.count(u"sync.difference"_s), 1);
}

void TestSync::resyncSignal() {
	auto harness = Harness();
	QVERIFY(Connect(harness, 100));
	harness.on(u"sync.difference"_s, [](const Call &) {
		return Reply::Ok(Difference(100, QJsonArray()));
	});
	harness.transport.emitFrame(Signal(Type::Resync));
	QVERIFY(harness.sync().updating());
	QVERIFY(Test::WaitFor([&] { return !harness.sync().updating(); }));
	QCOMPARE(DifferenceFrom(harness), QStringList{ u"100"_s });
	// A failed difference ends «updating» as well.
	harness.on(u"sync.difference"_s, [](const Call &) {
		return Reply::Offline();
	});
	harness.transport.emitFrame(Signal(Type::Resync));
	QVERIFY(Test::WaitFor([&] { return !harness.sync().updating(); }));
	QCOMPARE(harness.sync().pts().value_or(-1), 100);
}

void TestSync::acksEverySixteen() {
	auto harness = Harness();
	QVERIFY(Connect(harness, 100));
	harness.transport.acks.clear();
	auto pts = qint64(100);
	for (auto i = 0; i < Sync::kAckEvery - 1; ++i) {
		harness.transport.emitFrame(Update(++pts));
	}
	QVERIFY(harness.transport.acks.isEmpty());
	harness.transport.emitFrame(Update(++pts));
	QCOMPARE(harness.transport.acks, QVector<qint64>{ 116 });
	for (auto i = 0; i < Sync::kAckEvery; ++i) {
		harness.transport.emitFrame(Update(++pts));
	}
	QCOMPARE(harness.transport.acks, (QVector<qint64>{ 116, 132 }));
	QCOMPARE(harness.sync().pts().value_or(-1), 132);
}

void TestSync::updateKinds() {
	auto harness = Harness();
	QVERIFY(Connect(harness, 100));
	auto pts = qint64(100);
	const auto push = [&](const QString &kind, const QJsonObject &data) {
		harness.transport.emitFrame(Update(++pts, kind, data));
	};
	auto &messages = harness.messages();
	auto &chats = harness.chats();

	// read by the peer: my messages up to it are Read.
	QVERIFY(messages.find(u"c1"_s, 8)->mine);
	QVERIFY(messages.find(u"c1"_s, 8)->status == MessageStatus::Sent);
	push(u"read"_s, { { u"chat"_s, u"c1"_s }, { u"seq"_s, 8 }, { u"reader"_s, u"u_sara"_s } });
	QVERIFY(messages.find(u"c1"_s, 8)->status == MessageStatus::Read);
	QCOMPARE(chats.chat(u"c1"_s)->peerReadSeq, 8);
	QCOMPARE(chats.chat(u"c1"_s)->unread, 2);

	// read by me on another device: the badge clears.
	push(u"read"_s, { { u"chat"_s, u"c1"_s }, { u"seq"_s, 9 }, { u"reader"_s, u"u_me"_s } });
	QCOMPARE(chats.chat(u"c1"_s)->unread, 0);
	QCOMPARE(chats.chat(u"c1"_s)->lastReadSeq, 9);

	// edited
	auto edited = MessageJson(u"c1"_s, 7, u"u_sara"_s, u"ویرایش شد"_s);
	edited.insert(u"edited"_s, true);
	push(u"edited"_s, edited);
	QCOMPARE(messages.find(u"c1"_s, 7)->text, u"ویرایش شد"_s);
	QVERIFY(messages.find(u"c1"_s, 7)->edited);
	QCOMPARE(chats.chat(u"c1"_s)->lastSeq, 9); // not a new message

	// deleted: the last message goes, the row shows the one before it.
	auto deleted = MessageJson(u"c1"_s, 9, u"u_sara"_s, QString());
	deleted.insert(u"deleted"_s, true);
	push(u"deleted"_s, deleted);
	QVERIFY(messages.find(u"c1"_s, 9) == nullptr);
	QCOMPARE(chats.chat(u"c1"_s)->lastMessage->seq, 8);
	QVERIFY(chats.chat(u"c1"_s)->lastMine);

	// played
	push(u"played"_s, { { u"chat"_s, u"c1"_s }, { u"seq"_s, 6 } });
	QVERIFY(messages.find(u"c1"_s, 6)->played);

	// draft from another device, then cleared
	push(u"draft"_s, { { u"chat"_s, u"c2"_s }, { u"draft"_s, QJsonObject{ { u"text"_s, u"نیمه‌کاره"_s } } } });
	QVERIFY(chats.chat(u"c2"_s)->draft.has_value());
	QCOMPARE(chats.chat(u"c2"_s)->draft->text, u"نیمه‌کاره"_s);
	push(u"draft"_s, { { u"chat"_s, u"c2"_s } });
	QVERIFY(!chats.chat(u"c2"_s)->draft.has_value());

	// member: mute / archive
	push(u"member"_s, { { u"chat"_s, u"c2"_s }, { u"muted"_s, true }, { u"muted_until"_s, 1900000000 } });
	QVERIFY(chats.chat(u"c2"_s)->muted);
	QCOMPARE(chats.chat(u"c2"_s)->mutedUntil, 1900000000);
	QVERIFY(!chats.chat(u"c2"_s)->archived);
	push(u"member"_s, { { u"chat"_s, u"c2"_s }, { u"archived"_s, true } });
	QVERIFY(chats.chat(u"c2"_s)->archived);
	QVERIFY(chats.chat(u"c2"_s)->muted);

	// reaction: only the delta comes, the message is read once.
	const auto reads = harness.transport.count(u"chat.history"_s);
	push(u"reaction"_s, { { u"chat"_s, u"c1"_s }, { u"seq"_s, 5 }, { u"emoji"_s, u"🔥"_s } });
	Test::Settle();
	QCOMPARE(harness.transport.count(u"chat.history"_s), reads + 1);
	// ... and not at all for a message that is not on screen.
	push(u"reaction"_s, { { u"chat"_s, u"c1"_s }, { u"seq"_s, 500 }, { u"emoji"_s, u"🔥"_s } });
	Test::Settle();
	QCOMPARE(harness.transport.count(u"chat.history"_s), reads + 1);

	// cleared: the mark is the last message that went (the server writes the
	// chat's last seq there), so the history up to and including it goes.
	push(u"cleared"_s, { { u"chat"_s, u"c1"_s }, { u"cleared_before_seq"_s, 6 } });
	QVERIFY(messages.find(u"c1"_s, 5) == nullptr);
	QVERIFY(messages.find(u"c1"_s, 6) == nullptr);
	// Something newer is left: the chat stays in the list with it.
	QVERIFY(chats.chat(u"c1"_s) != nullptr);
	QCOMPARE(chats.chat(u"c1"_s)->lastMessage->seq, 8);

	// The whole history cleared: the chat leaves the list (the inbox does not
	// return it either until somebody writes again).
	QVERIFY(chats.chat(u"c2"_s) != nullptr);
	push(u"cleared"_s, { { u"chat"_s, u"c2"_s }, { u"cleared_before_seq"_s, chats.chat(u"c2"_s)->lastSeq } });
	QVERIFY(chats.chat(u"c2"_s) == nullptr);

	// notif: the bell.
	push(u"notif"_s, { { u"news"_s, 4 }, { u"kinds"_s, QJsonObject{ { u"like"_s, 3 }, { u"follow"_s, 1 } } } });
	QCOMPARE(harness.session->notifications().unread(), 4);

	// An unknown kind moves pts and changes nothing.
	push(u"something_new"_s, { { u"chat"_s, u"c1"_s } });
	QCOMPARE(harness.sync().pts().value_or(-1), pts);
	Test::Settle();
	QCOMPARE(harness.transport.count(u"sync.difference"_s), 0);
}

void TestSync::messageForUnloadedTimeline() {
	auto harness = Harness();
	QVERIFY(harness.start());
	harness.transport.setOnline(true);
	harness.transport.emitFrame(Hello(100));
	// c2 was never opened: the row follows, the timeline stays empty (it is
	// read whole when the chat opens - no hole between old and new).
	harness.transport.emitFrame(Update(101, u"message"_s, MessageJson(u"c2"_s, 5, u"u_ali"_s, u"سلام"_s)));
	QVERIFY(harness.messages().history(u"c2"_s).items.isEmpty());
	QCOMPARE(harness.chats().chat(u"c2"_s)->lastSeq, 5);
	QCOMPARE(harness.chats().chat(u"c2"_s)->lastMessage->text, u"سلام"_s);
	QCOMPARE(harness.chats().chat(u"c2"_s)->unread, 1);

	// A message of a chat we have no row for: the inbox is read again (once).
	const auto inboxLoads = harness.transport.count(u"chat.inbox"_s);
	auto reload = QSignalSpy(&harness.sync(), &Sync::inboxReloadRequested);
	harness.transport.emitFrame(Update(102, u"message"_s, MessageJson(u"c_new"_s, 1, u"u_x"_s, u"hi"_s)));
	harness.transport.emitFrame(Update(103, u"message"_s, MessageJson(u"c_new"_s, 2, u"u_x"_s, u"there"_s)));
	QVERIFY(Test::WaitFor([&] { return reload.size() == 1; }, 3000));
	Test::Settle(100);
	QCOMPARE(reload.size(), 1);
	QCOMPARE(harness.transport.count(u"chat.inbox"_s), inboxLoads + 3); // the three sections
}

void TestSync::transientFrames() {
	auto harness = Harness();
	QVERIFY(Connect(harness, 100));
	auto activity = QSignalSpy(&harness.chats(), &Chats::activityChanged);
	auto typing = Rt::Frame();
	typing.type = Type::Typing;
	typing.chat = u"c1"_s;
	typing.kind = u"recording_voice"_s;
	typing.ttl = 6;
	harness.transport.emitFrame(typing);
	QVERIFY(harness.chats().activity(u"c1"_s).has_value());
	QCOMPARE(harness.chats().activity(u"c1"_s)->kind, u"recording_voice"_s);
	QCOMPARE(activity.size(), 1);
	typing.kind = u"cancel"_s;
	harness.transport.emitFrame(typing);
	QVERIFY(!harness.chats().activity(u"c1"_s).has_value());

	auto presenceChanged = QSignalSpy(&harness.chats(), &Chats::presenceChanged);
	auto presence = Rt::Frame();
	presence.type = Type::Presence;
	presence.user = u"u_sara"_s;
	presence.status = u"online"_s;
	harness.transport.emitFrame(presence);
	QVERIFY(harness.chats().presence(u"u_sara"_s).has_value());
	QVERIFY(harness.chats().presence(u"u_sara"_s)->online());
	QCOMPARE(presenceChanged.size(), 1);
	presence.status = u"offline"_s;
	presence.lastSeenAt = 1800000000;
	harness.transport.emitFrame(presence);
	QVERIFY(!harness.chats().presence(u"u_sara"_s)->online());
	QCOMPARE(harness.chats().presence(u"u_sara"_s)->lastSeenAt, 1800000000);

	// None of these moves pts.
	QCOMPARE(harness.sync().pts().value_or(-1), 100);
}

void TestSync::ptsSurvivesRestart() {
	auto dir = QTemporaryDir();
	QVERIFY(dir.isValid());
	const auto path = dir.filePath(u"cache.db"_s);
	{
		auto harness = Harness(path);
		QVERIFY(Connect(harness, 100));
		harness.transport.emitFrame(Update(101, u"message"_s, MessageJson(u"c1"_s, 10, u"u_sara"_s, u"a"_s)));
		QCOMPARE(harness.sync().pts().value_or(-1), 101);
	}
	{
		auto harness = Harness(path);
		auto ready = false;
		harness.session->start([&] {
			ready = true;
		});
		QVERIFY(Test::WaitFor([&] { return ready; }));
		// The auth request of the next connection says where we are.
		QCOMPARE(harness.sync().pts().value_or(-1), 101);
		QCOMPARE(harness.transport.ptsProvider(), 101);
		// The cached row and message are back before the server is asked.
		auto count = -1;
		harness.messages().loadCached(u"c1"_s, base::Guard(), [&](int loaded) { count = loaded; });
		QVERIFY(Test::WaitFor([&] { return count >= 0; }));
		QVERIFY(count >= 10);
		QCOMPARE(harness.messages().find(u"c1"_s, 10)->text, u"a"_s);
		// A Hello behind us (another server node) does not move pts back.
		harness.transport.setOnline(true);
		harness.transport.emitFrame(Hello(90));
		QCOMPARE(harness.sync().pts().value_or(-1), 101);
	}
}

void TestSync::fallbackOnlyWhileDisconnected() {
	// The interval comes from the client config (five seconds is the floor).
	auto configServer = Test::HttpServer();
	configServer.handler = [](const Test::HttpServer::Request &) {
		return Test::HttpServer::Response::Ok(QJsonObject{
			{ u"realtime.fallback_sync_interval"_s, u"5s"_s },
		});
	};
	auto configApi = Api::Client(configServer.base(), u"1.0.0"_s);
	auto config = Api::ClientConfig(&configApi, nullptr);
	auto loaded = false;
	config.load(nullptr, [&](bool) { loaded = true; });
	QVERIFY(Test::WaitFor([&] { return loaded; }));
	QCOMPARE(config.fallbackSyncInterval(), 5);

	auto options = Harness::Options();
	options.clientConfig = &config;
	auto harness = Harness(options);
	harness.on(u"sync.difference"_s, [](const Call &) {
		return Reply::Ok(Difference(100, QJsonArray()));
	});
	QVERIFY(Connect(harness, 100));

	// Connected: the connection is the only source, nothing polls.
	QTest::qWait(5600);
	QCOMPARE(harness.transport.count(u"sync.difference"_s), 0);

	// The connection drops and stays down: the difference is read on a timer.
	harness.transport.setOnline(false);
	harness.transport.emitFrame(Signal(Type::Disconnected));
	QTest::qWait(4000);
	QCOMPARE(harness.transport.count(u"sync.difference"_s), 0);
	QVERIFY(Test::WaitFor([&] { return harness.transport.count(u"sync.difference"_s) == 1; }, 2500));
	QVERIFY(Test::WaitFor([&] { return harness.transport.count(u"sync.difference"_s) == 2; }, 6500));

	// Back up: the timer stops.
	harness.transport.setOnline(true);
	harness.transport.emitFrame(Hello(100));
	QTest::qWait(5600);
	QCOMPARE(harness.transport.count(u"sync.difference"_s), 2);
}

void TestSync::offlineStartReadsStateAtOnce() {
	// The socket never connects (the HTTP fallback): the client must know its
	// pts as soon as the session is ready, not at the first fallback tick,
	// or what happens in between is never read.
	auto harness = Harness();
	harness.on(u"sync.state"_s, [](const Call &) {
		return Reply::Ok(QJsonObject{ { u"pts"_s, u"300"_s } });
	});
	harness.on(u"sync.difference"_s, [](const Call &call) {
		return (call.body.value("pts"_L1).toString() == u"300")
			? Reply::Ok(Difference(301, QJsonArray{
				Item(301, MessageJson(u"c1"_s, 11, u"u_sara"_s, u"early"_s)),
			}))
			: Reply::Ok(Difference(301, QJsonArray()));
	});
	QVERIFY(harness.start());
	QVERIFY(Test::WaitFor([&] { return harness.sync().pts().has_value(); }, 1500));
	QCOMPARE(harness.sync().pts().value_or(-1), 300);
	QCOMPARE(harness.transport.count(u"sync.state"_s), 1);
	QCOMPARE(harness.transport.count(u"sync.difference"_s), 0);
	QVERIFY(!harness.sync().updating());
	QCOMPARE(harness.transport.ptsProvider(), 300);

	// An update that happened right after the start is read from that point
	// by the very first difference.
	auto loaded = false;
	harness.messages().loadLatest(u"c1"_s, base::Guard(), [&] { loaded = true; });
	QVERIFY(Test::WaitFor([&] { return loaded; }));
	harness.sync().catchUp();
	QVERIFY(Test::WaitFor([&] { return harness.sync().pts().value_or(-1) == 301; }));
	QCOMPARE(DifferenceFrom(harness), (QStringList{ u"300"_s }));
	const auto message = harness.messages().find(u"c1"_s, 11);
	QVERIFY(message != nullptr);
	QCOMPARE(message->text, u"early"_s);

	// The connection comes up later, ahead of us: the difference fills in.
	harness.transport.setOnline(true);
	harness.transport.emitFrame(Hello(305));
	QVERIFY(Test::WaitFor([&] { return harness.transport.count(u"sync.difference"_s) == 2; }));
	QCOMPARE(DifferenceFrom(harness).back(), u"301"_s);
	QCOMPARE(harness.transport.count(u"sync.state"_s), 1);
}

// Messages written while a chat is not open are never cached, so the
// newest page may begin far after the cached run: the run goes (memory and
// database) instead of leaving a hole that the cache would serve forever.
void TestSync::newestPageAttachesToTheCache() {
	auto dir = QTemporaryDir();
	QVERIFY(dir.isValid());
	const auto pathA = dir.filePath(u"a.db"_s);
	QVERIFY(CacheC1(pathA));
	{
		// 100 messages later.
		auto harness = Harness(pathA);
		harness.on(u"chat.history"_s, [](const Call &call) {
			const auto before = Param(call, "before");
			return Reply::Ok(!before
				? HistoryPage(60, 109, true, 109)
				: HistoryPage(std::max<qint64>(before - 50, 1), before - 1, before > 51, 109));
		});
		QVERIFY(harness.start());
		QVERIFY(OpenC1(harness));
		QCOMPARE(HeldSeqs(harness, u"c1"_s), Seqs(60, 109));
		QCOMPARE(CachedSeqs(harness, u"c1"_s), Seqs(60, 109));
		// Scrolling up reads the gap from the server, not the old run.
		auto added = -1;
		harness.messages().loadOlder(u"c1"_s, base::Guard(), [&](int count) { added = count; });
		QVERIFY(Test::WaitFor([&] { return added >= 0; }));
		QCOMPARE(added, 50);
		QCOMPARE(Param(harness.transport.calls[harness.transport.lastIndexOf(u"chat.history"_s)], "before"), 60);
		QCOMPARE(HeldSeqs(harness, u"c1"_s), Seqs(10, 109));
		QCOMPARE(CachedSeqs(harness, u"c1"_s), Seqs(10, 109));
	}
	const auto pathB = dir.filePath(u"b.db"_s);
	QVERIFY(CacheC1(pathB));
	{
		// A page that continues the cached run keeps it.
		auto harness = Harness(pathB);
		harness.on(u"chat.history"_s, [](const Call &) {
			return Reply::Ok(HistoryPage(10, 59, true, 59));
		});
		QVERIFY(harness.start());
		QVERIFY(OpenC1(harness));
		QCOMPARE(HeldSeqs(harness, u"c1"_s), Seqs(1, 59));
		QCOMPARE(CachedSeqs(harness, u"c1"_s), Seqs(1, 59));
	}
	const auto pathC = dir.filePath(u"c.db"_s);
	QVERIFY(CacheC1(pathC));
	{
		// Nothing before the page (1..5 are no longer mine to see): what is
		// cached before it goes.
		auto harness = Harness(pathC);
		harness.on(u"chat.history"_s, [](const Call &) {
			return Reply::Ok(HistoryPage(6, 9, false, 9));
		});
		QVERIFY(harness.start());
		QVERIFY(OpenC1(harness));
		QCOMPARE(HeldSeqs(harness, u"c1"_s), Seqs(6, 9));
		QCOMPARE(CachedSeqs(harness, u"c1"_s), Seqs(6, 9));
		QVERIFY(!harness.messages().history(u"c1"_s).hasMoreBefore);
	}
	const auto pathD = dir.filePath(u"d.db"_s);
	QVERIFY(CacheC1(pathD));
	{
		// An empty newest page: nothing is left up to the header's last seq.
		auto harness = Harness(pathD);
		harness.on(u"chat.history"_s, [](const Call &) {
			return Reply::Ok(HistoryPage(1, 0, false, 9));
		});
		QVERIFY(harness.start());
		QVERIFY(OpenC1(harness));
		QVERIFY(HeldSeqs(harness, u"c1"_s).isEmpty());
		QVERIFY(CachedSeqs(harness, u"c1"_s).isEmpty());
	}
}

// A window around a message that reaches the newest one (at once, or by
// reading newer pages) is what live messages continue: it replaces a cached
// run it does not reach and is cached itself.
void TestSync::windowReachingTheEndIsCached() {
	auto dir = QTemporaryDir();
	QVERIFY(dir.isValid());
	const auto pathE = dir.filePath(u"e.db"_s);
	QVERIFY(CacheC1(pathE));
	{
		auto harness = Harness(pathE);
		harness.on(u"chat.history"_s, [](const Call &) {
			return Reply::Ok(HistoryPage(75, 120, true, 120, false));
		});
		QVERIFY(harness.start());
		harness.transport.setOnline(true);
		harness.transport.emitFrame(Hello(100));
		auto cached = false;
		harness.messages().loadCached(u"c1"_s, base::Guard(), [&](int) { cached = true; });
		QVERIFY(Test::WaitFor([&] { return cached; }));
		auto done = false;
		harness.messages().loadAround(u"c1"_s, 100, base::Guard(), [&] { done = true; });
		QVERIFY(Test::WaitFor([&] { return done; }));
		QVERIFY(harness.messages().live(u"c1"_s));
		harness.transport.emitFrame(Update(101, u"message"_s, MessageJson(u"c1"_s, 121, u"u_sara"_s, u"new"_s)));
		QCOMPARE(HeldSeqs(harness, u"c1"_s), Seqs(75, 121));
		QCOMPARE(CachedSeqs(harness, u"c1"_s), Seqs(75, 121));
	}
	const auto pathF = dir.filePath(u"f.db"_s);
	QVERIFY(CacheC1(pathF));
	{
		auto harness = Harness(pathF);
		harness.on(u"chat.history"_s, [](const Call &call) {
			return Reply::Ok(Param(call, "after")
				? HistoryPage(126, 130, true, 130, false)
				: HistoryPage(75, 125, true, 130, true));
		});
		QVERIFY(harness.start());
		auto done = false;
		harness.messages().loadAround(u"c1"_s, 100, base::Guard(), [&] { done = true; });
		QVERIFY(Test::WaitFor([&] { return done; }));
		QVERIFY(!harness.messages().live(u"c1"_s));
		// A detached window is not cached.
		QCOMPARE(CachedSeqs(harness, u"c1"_s), Seqs(1, 9));
		auto added = -1;
		harness.messages().loadNewer(u"c1"_s, base::Guard(), [&](int count) { added = count; });
		QVERIFY(Test::WaitFor([&] { return added >= 0; }));
		QCOMPARE(added, 5);
		QVERIFY(harness.messages().live(u"c1"_s));
		QCOMPARE(CachedSeqs(harness, u"c1"_s), Seqs(75, 130));
	}
}

// An edit or a deletion of a message that is cached but not in the window
// (the chat is not open, or the message is older than what is shown)
// changes the cached copy; a new message for a timeline that does not reach
// the end is not cached, and an old one does not join the window.
void TestSync::changesOutsideTheWindowReachTheCache() {
	auto dir = QTemporaryDir();
	QVERIFY(dir.isValid());
	const auto path = dir.filePath(u"a.db"_s);
	QVERIFY(CacheC1(path));
	const auto edited = [](qint64 seq) {
		auto result = MessageJson(u"c1"_s, seq, u"u_sara"_s, u"ویرایش شد"_s);
		result.insert(u"edited"_s, true);
		return result;
	};
	{
		auto harness = Harness(path);
		QVERIFY(harness.start());
		harness.transport.setOnline(true);
		harness.transport.emitFrame(Hello(100));
		QVERIFY(!harness.messages().held(u"c1"_s));
		harness.transport.emitFrame(Update(101, u"edited"_s, edited(7)));
		auto deleted = MessageJson(u"c1"_s, 8, u"u_sara"_s, QString());
		deleted.insert(u"deleted"_s, true);
		harness.transport.emitFrame(Update(102, u"deleted"_s, deleted));
		harness.transport.emitFrame(Update(103, u"message"_s, MessageJson(u"c1"_s, 10, u"u_sara"_s, u"new"_s)));
		QCOMPARE(CachedSeqs(harness, u"c1"_s), (QVector<qint64>{ 1, 2, 3, 4, 5, 6, 7, 9 }));
		const auto seven = CachedMessage(harness, u"c1"_s, 7);
		QVERIFY(seven.has_value());
		QCOMPARE(seven->text, u"ویرایش شد"_s);
		QVERIFY(seven->edited);
		// Opened later, the cache shows them as they are.
		auto count = -1;
		harness.messages().loadCached(u"c1"_s, base::Guard(), [&](int loaded) { count = loaded; });
		QVERIFY(Test::WaitFor([&] { return count >= 0; }));
		QCOMPARE(harness.messages().find(u"c1"_s, 7)->text, u"ویرایش شد"_s);
		QVERIFY(harness.messages().find(u"c1"_s, 8) == nullptr);
	}
	{
		// On screen, the newest page shows 5..9 only.
		auto harness = Harness(path);
		harness.on(u"chat.history"_s, [](const Call &) {
			return Reply::Ok(HistoryPage(5, 9, true, 9));
		});
		QVERIFY(harness.start());
		auto loaded = false;
		harness.messages().loadLatest(u"c1"_s, base::Guard(), [&] { loaded = true; });
		QVERIFY(Test::WaitFor([&] { return loaded; }));
		harness.transport.setOnline(true);
		harness.transport.emitFrame(Hello(103));
		harness.transport.emitFrame(Update(104, u"edited"_s, edited(2)));
		QCOMPARE(HeldSeqs(harness, u"c1"_s), Seqs(5, 9));
		QCOMPARE(CachedMessage(harness, u"c1"_s, 2)->text, u"ویرایش شد"_s);
		// A message for me alone hidden on another device, outside the window.
		harness.transport.emitFrame(Update(105, u"member"_s, { { u"chat"_s, u"c1"_s }, { u"hidden_seq"_s, 3 } }));
		QVERIFY(!CachedMessage(harness, u"c1"_s, 3).has_value());
	}
}

// In a difference (and a replay) "cleared" and "member" come bare: the
// chat, the seq and the header. cleared: the seq is the chat's last one
// when it was cleared for both. member with a seq: that message was
// deleted for me; without one: anything of mine, a history cleared for me
// included, so what is cached of the chat is read again.
void TestSync::differenceItemsWithoutPayload() {
	auto dir = QTemporaryDir();
	QVERIFY(dir.isValid());
	auto harness = Harness(dir.filePath(u"cache.db"_s));
	QVERIFY(Connect(harness, 100));
	const auto header = [](const char *fixture) {
		return Test::Fixture(fixture).value("chat"_L1).toObject();
	};
	const auto bare = [](qint64 pts, const QString &kind, const QString &chat, qint64 seq, const QJsonObject &info) {
		auto result = QJsonObject{
			{ u"pts"_s, QString::number(pts) },
			{ u"kind"_s, kind },
			{ u"chat"_s, chat },
			{ u"chat_info"_s, info },
		};
		if (seq) {
			result.insert(u"seq"_s, QString::number(seq));
		}
		return result;
	};
	auto &chats = harness.chats();
	auto &messages = harness.messages();
	QCOMPARE(chats.chat(u"c1"_s)->lastMessage->seq, 9);
	harness.on(u"sync.difference"_s, [&](const Call &call) {
		return Reply::Ok((call.body.value("pts"_L1).toString() == u"100")
			? Difference(102, QJsonArray{
				bare(101, u"member"_s, u"c1"_s, 9, header("history_c1.json")),
				bare(102, u"cleared"_s, u"c2"_s, 4, header("history_c2.json")),
			})
			: Difference(102, QJsonArray()));
	});
	harness.sync().catchUp();
	QVERIFY(Test::WaitFor([&] { return !harness.sync().updating(); }));
	QCOMPARE(harness.sync().pts().value_or(-1), 102);
	QVERIFY(messages.find(u"c1"_s, 9) == nullptr);
	QCOMPARE(CachedSeqs(harness, u"c1"_s), Seqs(1, 8));
	QCOMPARE(chats.chat(u"c1"_s)->lastMessage->seq, 8);
	// c2 was cleared to its end: it leaves the list.
	QVERIFY(chats.chat(u"c2"_s) == nullptr);

	// Muted, or my history cleared: the item does not say which.
	const auto inboxLoads = harness.transport.count(u"chat.inbox"_s);
	const auto pages = harness.transport.count(u"chat.history"_s);
	harness.on(u"sync.difference"_s, [&](const Call &call) {
		return Reply::Ok((call.body.value("pts"_L1).toString() == u"102")
			? Difference(103, QJsonArray{ bare(103, u"member"_s, u"c1"_s, 0, header("history_c1.json")) })
			: Difference(103, QJsonArray()));
	});
	harness.sync().catchUp();
	QVERIFY(Test::WaitFor([&] { return harness.sync().pts().value_or(-1) == 103 && !harness.sync().updating(); }));
	QVERIFY(CachedSeqs(harness, u"c1"_s).isEmpty());
	QVERIFY(messages.history(u"c1"_s).stale);
	QVERIFY(!messages.live(u"c1"_s));
	// Not on screen: read again when it opens.
	QCOMPARE(harness.transport.count(u"chat.history"_s), pages);
	QVERIFY(Test::WaitFor([&] { return harness.transport.count(u"chat.inbox"_s) > inboxLoads; }, 3000));
	auto loaded = false;
	harness.on(u"chat.history"_s, [](const Call &) {
		return Reply::Ok(HistoryPage(8, 8, false, 9));
	});
	messages.loadLatest(u"c1"_s, base::Guard(), [&] { loaded = true; });
	QVERIFY(Test::WaitFor([&] { return loaded; }));
	QCOMPARE(HeldSeqs(harness, u"c1"_s), (QVector<qint64>{ 8 }));
	QVERIFY(!messages.history(u"c1"_s).stale);

	// On screen: read again at once.
	chats.setVisibleChat(u"c1"_s);
	const auto before = harness.transport.count(u"chat.history"_s);
	harness.on(u"sync.difference"_s, [&](const Call &call) {
		return Reply::Ok((call.body.value("pts"_L1).toString() == u"103")
			? Difference(104, QJsonArray{ bare(104, u"member"_s, u"c1"_s, 0, header("history_c1.json")) })
			: Difference(104, QJsonArray()));
	});
	harness.sync().catchUp();
	QVERIFY(Test::WaitFor([&] { return harness.transport.count(u"chat.history"_s) > before; }));
	QVERIFY(Test::WaitFor([&] { return messages.live(u"c1"_s); }));
	QCOMPARE(HeldSeqs(harness, u"c1"_s), (QVector<qint64>{ 8 }));
}

QTEST_GUILESS_MAIN(TestSync)
#include "test_sync.moc"
