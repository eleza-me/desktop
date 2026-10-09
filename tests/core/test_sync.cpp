// Data::Sync on a fake transport: which frames are applied, when the
// difference is read, what is acknowledged, and that nothing polls while the
// connection is up.
#include "api/api_client_config.h"
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

} // namespace

class TestSync final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void firstHelloAdoptsServerPts();
	void liveUpdatesApplyInOrder();
	void gapReadsTheDifference();
	void differenceIsPaged();
	void tooLongStartsOver();
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

};

void TestSync::firstHelloAdoptsServerPts() {
	auto harness = Harness();
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
	QVERIFY(harness.messages().history(u"c1"_s).loaded);
	harness.transport.emitFrame(Signal(Type::Resync));
	QVERIFY(Test::WaitFor([&] { return !harness.sync().updating(); }));
	Test::Settle();
	QCOMPARE(harness.sync().pts().value_or(-1), 520);
	QCOMPARE(harness.transport.count(u"sync.state"_s), 1);
	QCOMPARE(DifferenceFrom(harness), (QStringList{ u"100"_s, u"520"_s }));
	// Everything cached is read again: the inbox now, timelines when opened.
	QVERIFY(harness.transport.count(u"chat.inbox"_s) > inboxLoads);
	QVERIFY(!harness.messages().history(u"c1"_s).loaded);
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

	// cleared: history before a seq goes.
	push(u"cleared"_s, { { u"chat"_s, u"c1"_s }, { u"cleared_before_seq"_s, 6 } });
	QVERIFY(messages.find(u"c1"_s, 5) == nullptr);
	QVERIFY(messages.find(u"c1"_s, 6) != nullptr);

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

QTEST_GUILESS_MAIN(TestSync)
#include "test_sync.moc"
