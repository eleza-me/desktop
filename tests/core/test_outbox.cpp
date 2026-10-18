// Data::Outbox: what is queued, in which order it leaves, what happens on
// failures, and that the queue survives a restart.
#include "base/random.h"
#include "data/data_json.h"
#include "test_harness.h"

#include <QtCore/QTemporaryDir>
#include <QtTest/QSignalSpy>

using namespace Data;
using Test::Harness;
using Reply = Test::FakeTransport::Reply;
using Call = Test::FakeTransport::Call;

namespace {

[[nodiscard]] QStringList SentTexts(const Harness &harness, const QString &chat) {
	auto result = QStringList();
	for (const auto &body : harness.bodies(u"chat.send"_s)) {
		if (body.value("chat"_L1).toString() == chat) {
			result.push_back(body.value("text"_L1).toString());
		}
	}
	return result;
}

[[nodiscard]] MessageStatus StatusOf(Harness &harness, const QString &chat, const QString &id) {
	const auto &history = harness.messages().history(chat);
	const auto index = history.indexOfKey(Message::KeyForLocal(id));
	return (index < 0) ? MessageStatus::Sent : history.items[index].status;
}

[[nodiscard]] bool HasLocal(Harness &harness, const QString &chat, const QString &id) {
	return harness.messages().history(chat).indexOfKey(Message::KeyForLocal(id)) >= 0;
}

[[nodiscard]] QJsonObject Sent(qint64 seq) {
	return QJsonObject{
		{ u"seq"_s, QString::number(seq) },
		{ u"message_id"_s, u"m%1"_s.arg(seq) },
		{ u"created_at"_s, QString::number(1800000000000 + seq) },
	};
}

} // namespace

class TestOutbox final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void textIsShownThenSent();
	void orderWithinChat();
	void transientFailureKeepsOrder();
	void backoffRetriesByItself();
	void permanentFailureLetsOthersPass();
	void attemptsRunOut();
	void discardRemovesEverything();
	void bodiesOfEveryKind();
	void echoBeforeAnswer();
	void fileIsUploadedFirst();
	void processedSignalEndsTheWait();
	void queueSurvivesRestart();

};

void TestOutbox::textIsShownThenSent() {
	auto harness = Harness();
	QVERIFY(harness.start());
	harness.on(u"chat.send"_s, [](const Call &) {
		return Reply::Ok(Sent(10));
	});
	auto sent = QSignalSpy(&harness.outbox(), &Outbox::sent);

	auto options = SendTextOptions();
	options.replyToSeq = 3;
	auto bold = Entity();
	bold.type = u"bold"_s;
	bold.offset = 0;
	bold.length = 4;
	options.entities.push_back(bold);
	const auto id = harness.outbox().sendText(u"c1"_s, u"سلام دنیا"_s, options);
	QVERIFY(!id.isEmpty());

	// Before the event loop turns: the bubble is there and the row shows it.
	const auto &history = harness.messages().history(u"c1"_s);
	const auto index = history.indexOfKey(Message::KeyForLocal(id));
	QVERIFY(index >= 0);
	const auto local = history.items[index];
	QVERIFY(local.local());
	QVERIFY(local.mine);
	QVERIFY(local.status == MessageStatus::Sending);
	QCOMPARE(local.text, u"سلام دنیا"_s);
	QCOMPARE(local.replyToSeq, 3);
	QCOMPARE(local.entities.size(), 1);
	QCOMPARE(local.sender, u"u_me"_s);
	const auto row = harness.chats().chat(u"c1"_s);
	QVERIFY(row && row->lastMessage);
	QCOMPARE(row->lastMessage->clientMessageId, id);
	QVERIFY(row->lastMine);
	QCOMPARE(harness.outbox().pendingCount(), 1);
	QCOMPARE(harness.outbox().pendingCount(u"c2"_s), 0);

	QVERIFY(Test::WaitFor([&] { return sent.size() == 1; }));
	QCOMPARE(sent[0][0].toString(), u"c1"_s);
	QCOMPARE(sent[0][1].toString(), id);
	QCOMPARE(sent[0][2].toLongLong(), 10);

	const auto index2 = harness.transport.lastIndexOf(u"chat.send"_s);
	const auto call = harness.transport.calls[index2];
	QCOMPARE(harness.transport.count(u"chat.send"_s), 1);
	QCOMPARE(call.idempotencyKey, id);
	QCOMPARE(call.body.value("chat"_L1).toString(), u"c1"_s);
	QCOMPARE(call.body.value("client_message_id"_L1).toString(), id);
	QCOMPARE(call.body.value("kind"_L1).toString(), u"text"_s);
	QCOMPARE(call.body.value("text"_L1).toString(), u"سلام دنیا"_s);
	QCOMPARE(Json::Int64(call.body, "reply_to_seq"_L1), 3);
	QCOMPARE(call.body.value("entities"_L1).toArray().size(), 1);
	QVERIFY(!call.body.contains("quote"_L1));
	QVERIFY(!call.body.contains("link_preview"_L1));

	// The bubble became the server's message.
	QVERIFY(!HasLocal(harness, u"c1"_s, id));
	const auto server = harness.messages().find(u"c1"_s, 10);
	QVERIFY(server);
	QCOMPARE(server->clientMessageId, id);
	QCOMPARE(server->id, u"m10"_s);
	QCOMPARE(server->createdAt, 1800000000010);
	QVERIFY(server->status == MessageStatus::Sent);
	QVERIFY(harness.outbox().items().isEmpty());
	QVERIFY(!harness.outbox().contains(id));
	QCOMPARE(harness.chats().chat(u"c1"_s)->lastSeq, 10);
}

void TestOutbox::orderWithinChat() {
	auto harness = Harness();
	QVERIFY(harness.start());
	harness.on(u"chat.send"_s, [](const Call &) {
		return Reply::Hold();
	});
	auto &outbox = harness.outbox();
	const auto a = outbox.sendText(u"c1"_s, u"a"_s);
	const auto b = outbox.sendText(u"c1"_s, u"b"_s);
	const auto other = outbox.sendText(u"c2"_s, u"x"_s);
	const auto c = outbox.sendText(u"c1"_s, u"c"_s);
	Test::Settle();

	// One message per chat is on its way; chats do not wait for each other.
	QCOMPARE(SentTexts(harness, u"c1"_s), QStringList{ u"a"_s });
	QCOMPARE(SentTexts(harness, u"c2"_s), QStringList{ u"x"_s });
	QVERIFY(StatusOf(harness, u"c1"_s, a) == MessageStatus::Sending);
	QVERIFY(StatusOf(harness, u"c1"_s, b) == MessageStatus::Queued);
	QVERIFY(StatusOf(harness, u"c1"_s, c) == MessageStatus::Queued);
	QVERIFY(StatusOf(harness, u"c2"_s, other) == MessageStatus::Sending);
	QCOMPARE(outbox.pendingCount(u"c1"_s), 3);

	// The queue reports the order of queueing, with increasing times.
	const auto items = outbox.items();
	QCOMPARE(items.size(), 4);
	QCOMPARE(items[0].clientMessageId, a);
	QCOMPARE(items[1].clientMessageId, b);
	QCOMPARE(items[2].clientMessageId, other);
	QCOMPARE(items[3].clientMessageId, c);
	for (auto i = 1; i < items.size(); ++i) {
		QVERIFY(items[i].createdAt > items[i - 1].createdAt);
	}

	// The timeline shows them in the same order.
	const auto &history = harness.messages().history(u"c1"_s);
	QVERIFY(history.indexOfKey(Message::KeyForLocal(a)) < history.indexOfKey(Message::KeyForLocal(b)));
	QVERIFY(history.indexOfKey(Message::KeyForLocal(b)) < history.indexOfKey(Message::KeyForLocal(c)));

	harness.transport.resolve(harness.transport.lastIndexOf(u"chat.send"_s) - 1, Sent(10)); // a
	Test::Settle();
	QCOMPARE(SentTexts(harness, u"c1"_s), (QStringList{ u"a"_s, u"b"_s }));
	QVERIFY(StatusOf(harness, u"c1"_s, b) == MessageStatus::Sending);
	QVERIFY(StatusOf(harness, u"c1"_s, c) == MessageStatus::Queued);

	harness.transport.resolve(harness.transport.lastIndexOf(u"chat.send"_s), Sent(11)); // b
	Test::Settle();
	QCOMPARE(SentTexts(harness, u"c1"_s), (QStringList{ u"a"_s, u"b"_s, u"c"_s }));
	harness.transport.resolve(harness.transport.lastIndexOf(u"chat.send"_s), Sent(12)); // c
	Test::Settle();
	QCOMPARE(outbox.pendingCount(u"c1"_s), 0);
	QCOMPARE(outbox.pendingCount(u"c2"_s), 1);
	QCOMPARE(harness.messages().find(u"c1"_s, 10)->text, u"a"_s);
	QCOMPARE(harness.messages().find(u"c1"_s, 11)->text, u"b"_s);
	QCOMPARE(harness.messages().find(u"c1"_s, 12)->text, u"c"_s);
}

void TestOutbox::transientFailureKeepsOrder() {
	auto harness = Harness();
	QVERIFY(harness.start());
	auto offline = true;
	auto seq = qint64(20);
	harness.on(u"chat.send"_s, [&](const Call &) {
		return offline ? Reply::Offline() : Reply::Ok(Sent(seq++));
	});
	auto failed = QSignalSpy(&harness.outbox(), &Outbox::failed);
	auto &outbox = harness.outbox();
	const auto a = outbox.sendText(u"c1"_s, u"a"_s);
	const auto b = outbox.sendText(u"c1"_s, u"b"_s);
	Test::Settle(60);

	// `a` failed once and waits out its backoff; `b` must not overtake it.
	QCOMPARE(SentTexts(harness, u"c1"_s), QStringList{ u"a"_s });
	QCOMPARE(failed.size(), 0);
	QCOMPARE(outbox.items()[0].attempts, 1);
	QVERIFY(!outbox.items()[0].failed);
	QVERIFY(HasLocal(harness, u"c1"_s, a));
	QVERIFY(StatusOf(harness, u"c1"_s, a) != MessageStatus::Failed);

	// The connection came back (Sync calls retryAll on Hello).
	offline = false;
	outbox.retryAll();
	QVERIFY(Test::WaitFor([&] { return outbox.pendingCount() == 0; }));
	QCOMPARE(SentTexts(harness, u"c1"_s), (QStringList{ u"a"_s, u"a"_s, u"b"_s }));
	const auto calls = harness.transport.calls;
	auto keys = QStringList();
	for (const auto &call : calls) {
		if (call.method == u"chat.send") {
			keys.push_back(call.idempotencyKey);
		}
	}
	// The repeat carries the same id: the server dedups.
	QCOMPARE(keys, (QStringList{ a, a, b }));
	QCOMPARE(harness.messages().find(u"c1"_s, 20)->text, u"a"_s);
	QCOMPARE(harness.messages().find(u"c1"_s, 21)->text, u"b"_s);
}

void TestOutbox::backoffRetriesByItself() {
	auto harness = Harness();
	QVERIFY(harness.start());
	auto attempt = 0;
	harness.on(u"chat.send"_s, [&](const Call &) {
		return (++attempt == 1) ? Reply::Fail(503, u"server.unavailable"_s) : Reply::Ok(Sent(30));
	});
	auto sent = QSignalSpy(&harness.outbox(), &Outbox::sent);
	auto timer = QElapsedTimer();
	timer.start();
	harness.outbox().sendText(u"c1"_s, u"again"_s);
	QVERIFY(Test::WaitFor([&] { return sent.size() == 1; }, 6000));
	// The first retry comes two seconds after the failure.
	QVERIFY2(timer.elapsed() >= 1900, qPrintable(QString::number(timer.elapsed())));
	QVERIFY(timer.elapsed() < 4000);
	QCOMPARE(attempt, 2);
}

void TestOutbox::permanentFailureLetsOthersPass() {
	auto harness = Harness();
	QVERIFY(harness.start());
	auto refuse = true;
	auto seq = qint64(40);
	harness.on(u"chat.send"_s, [&](const Call &call) {
		return (refuse && call.body.value("text"_L1).toString() == u"a")
			? Reply::Fail(403, u"chat.blocked"_s)
			: Reply::Ok(Sent(seq++));
	});
	auto failed = QSignalSpy(&harness.outbox(), &Outbox::failed);
	auto &outbox = harness.outbox();
	const auto a = outbox.sendText(u"c1"_s, u"a"_s);
	const auto b = outbox.sendText(u"c1"_s, u"b"_s);
	QVERIFY(Test::WaitFor([&] { return harness.messages().find(u"c1"_s, 40) != nullptr; }));

	QCOMPARE(failed.size(), 1);
	QCOMPARE(failed[0][0].toString(), u"c1"_s);
	QCOMPARE(failed[0][1].toString(), a);
	QCOMPARE(failed[0][2].value<Api::Error>().code, u"chat.blocked"_s);
	QCOMPARE(failed[0][2].value<Api::Error>().status, 403);
	QVERIFY(StatusOf(harness, u"c1"_s, a) == MessageStatus::Failed);
	const auto &history = harness.messages().history(u"c1"_s);
	QCOMPARE(history.items[history.indexOfKey(Message::KeyForLocal(a))].errorCode, u"chat.blocked"_s);
	QCOMPARE(harness.messages().find(u"c1"_s, 40)->text, u"b"_s);
	QVERIFY(!HasLocal(harness, u"c1"_s, b));
	// A failed message is not pending, but it is still in the queue.
	QCOMPARE(outbox.pendingCount(), 0);
	QCOMPARE(outbox.items().size(), 1);
	QVERIFY(outbox.items()[0].failed);
	QVERIFY(outbox.contains(a));

	// retryAll leaves a failed message alone ...
	outbox.retryAll();
	Test::Settle();
	QCOMPARE(harness.transport.count(u"chat.send"_s), 2);
	// ... the user's retry sends it.
	refuse = false;
	outbox.retry(a);
	QVERIFY(StatusOf(harness, u"c1"_s, a) != MessageStatus::Failed);
	QVERIFY(Test::WaitFor([&] { return !outbox.contains(a); }));
	QCOMPARE(harness.messages().find(u"c1"_s, 41)->text, u"a"_s);
	QCOMPARE(harness.transport.count(u"chat.send"_s), 3);
}

void TestOutbox::attemptsRunOut() {
	auto harness = Harness();
	QVERIFY(harness.start());
	harness.on(u"chat.send"_s, [](const Call &) {
		return Reply::Offline();
	});
	auto failed = QSignalSpy(&harness.outbox(), &Outbox::failed);
	auto &outbox = harness.outbox();
	const auto a = outbox.sendText(u"c1"_s, u"a"_s);
	for (auto attempt = 1; attempt < Outbox::kMaxAttempts; ++attempt) {
		QVERIFY(Test::WaitFor([&] { return harness.transport.count(u"chat.send"_s) == attempt; }));
		Test::Settle();
		QCOMPARE(failed.size(), 0);
		QCOMPARE(outbox.items()[0].attempts, attempt);
		outbox.retryAll(); // skips the backoff
	}
	QVERIFY(Test::WaitFor([&] { return failed.size() == 1; }));
	QCOMPARE(harness.transport.count(u"chat.send"_s), int(Outbox::kMaxAttempts));
	QVERIFY(StatusOf(harness, u"c1"_s, a) == MessageStatus::Failed);
	QVERIFY(failed[0][2].value<Api::Error>().offline());
	// A manual retry starts the count again.
	outbox.retry(a);
	QCOMPARE(outbox.items()[0].attempts, 0);
	QVERIFY(!outbox.items()[0].failed);
}

void TestOutbox::discardRemovesEverything() {
	auto harness = Harness();
	QVERIFY(harness.start());
	harness.on(u"chat.send"_s, [](const Call &) {
		return Reply::Hold();
	});
	auto &outbox = harness.outbox();
	const auto a = outbox.sendText(u"c1"_s, u"a"_s);
	const auto b = outbox.sendText(u"c1"_s, u"b"_s);
	const auto c = outbox.sendText(u"c1"_s, u"c"_s);
	Test::Settle();
	QCOMPARE(SentTexts(harness, u"c1"_s), QStringList{ u"a"_s });

	// A queued one in the middle.
	outbox.discard(b);
	QVERIFY(!outbox.contains(b));
	QVERIFY(!HasLocal(harness, u"c1"_s, b));
	QCOMPARE(outbox.items().size(), 2);

	// The one on its way: the next one starts, and the late answer is ignored.
	outbox.discard(a);
	QVERIFY(!HasLocal(harness, u"c1"_s, a));
	Test::Settle();
	QCOMPARE(SentTexts(harness, u"c1"_s), (QStringList{ u"a"_s, u"c"_s }));
	harness.transport.resolve(harness.transport.lastIndexOf(u"chat.send"_s) - 1, Sent(50));
	Test::Settle();
	QVERIFY(harness.messages().find(u"c1"_s, 50) == nullptr);
	QVERIFY(outbox.contains(c));
	outbox.discard(u"no-such-id"_s);
	QCOMPARE(outbox.items().size(), 1);
}

void TestOutbox::bodiesOfEveryKind() {
	auto harness = Harness();
	QVERIFY(harness.start());
	auto seq = qint64(60);
	harness.on(u"chat.send"_s, [&](const Call &) {
		return Reply::Ok(Sent(seq++));
	});
	auto &outbox = harness.outbox();
	const auto last = [&] {
		Test::Settle();
		return harness.transport.calls[harness.transport.lastIndexOf(u"chat.send"_s)].body;
	};

	// A quote only travels with a reply.
	auto quote = Quote();
	quote.text = u"فردا"_s;
	quote.offset = 9;
	auto text = SendTextOptions();
	text.quote = quote;
	text.linkPreview = u"https://eleza.me/x"_s;
	outbox.sendText(u"c3"_s, u"t"_s, text);
	auto body = last();
	QVERIFY(!body.contains("quote"_L1));
	QCOMPARE(body.value("link_preview"_L1).toString(), u"https://eleza.me/x"_s);
	text.replyToSeq = 3;
	outbox.sendText(u"c3"_s, u"t"_s, text);
	body = last();
	QCOMPARE(body.value("quote"_L1).toObject().value("text"_L1).toString(), u"فردا"_s);

	QVERIFY(!outbox.sendSticker(u"c3"_s, u"st_1"_s, 2).isEmpty());
	body = last();
	QCOMPARE(body.value("kind"_L1).toString(), u"sticker"_s);
	QCOMPARE(body.value("sticker"_L1).toString(), u"st_1"_s);
	QCOMPARE(Json::Int64(body, "reply_to_seq"_L1), 2);
	QVERIFY(!body.contains("text"_L1));

	auto place = Location();
	place.lat = 35.6892;
	place.lng = 51.389;
	outbox.sendLocation(u"c3"_s, place);
	body = last();
	QCOMPARE(body.value("kind"_L1).toString(), u"location"_s);
	QCOMPARE(body.value("location"_L1).toObject().value("lat"_L1).toDouble(), 35.6892);
	QCOMPARE(body.value("location"_L1).toObject().value("lng"_L1).toDouble(), 51.389);

	const auto live = outbox.sendLiveLocation(u"c3"_s, place, 900);
	{
		const auto &history = harness.messages().history(u"c3"_s);
		const auto index = history.indexOfKey(Message::KeyForLocal(live));
		QVERIFY(index >= 0);
		QVERIFY(history.items[index].live.has_value());
		QCOMPARE(history.items[index].live->period, 900);
	}
	body = last();
	QCOMPARE(body.value("kind"_L1).toString(), u"live_location"_s);
	QCOMPARE(Json::Int64(body, "live_period"_L1), 900);

	outbox.sendPost(u"c3"_s, u"p1"_s);
	body = last();
	QCOMPARE(body.value("kind"_L1).toString(), u"forwarded_post"_s);
	QCOMPARE(body.value("forwarded_post"_L1).toString(), u"p1"_s);

	outbox.sendInlineResult(u"c3"_s, u"u_bot"_s, u"q1"_s, u"r7"_s);
	body = last();
	QVERIFY(!body.contains("kind"_L1));
	QVERIFY(!body.contains("text"_L1));
	const auto result = body.value("inline_result"_L1).toObject();
	QCOMPARE(result.value("bot"_L1).toString(), u"u_bot"_s);
	QCOMPARE(result.value("query_id"_L1).toString(), u"q1"_s);
	QCOMPARE(result.value("result_id"_L1).toString(), u"r7"_s);

	// Forwarding: media keeps its kind and is referenced, not uploaded again.
	const auto source = Json::List<Message>(Test::Fixture("history_c1.json").value("messages"_L1));
	const auto photo = std::find_if(source.cbegin(), source.cend(), [](const Message &message) {
		return message.kind == MessageKind::Photo;
	});
	QVERIFY(photo != source.cend());
	const auto uploads = harness.http.count("/v1/uploads");
	const auto forwarded = outbox.forward(u"c3"_s, *photo);
	QVERIFY(!forwarded.isEmpty());
	{
		const auto &history = harness.messages().history(u"c3"_s);
		const auto index = history.indexOfKey(Message::KeyForLocal(forwarded));
		QVERIFY(index >= 0);
		QVERIFY(history.items[index].forwarded);
		QVERIFY(history.items[index].kind == MessageKind::Photo);
		QCOMPARE(history.items[index].media->width, 1200);
	}
	body = last();
	QCOMPARE(body.value("kind"_L1).toString(), u"photo"_s);
	QCOMPARE(body.value("forward_from"_L1).toObject().value("chat"_L1).toString(), u"c1"_s);
	QCOMPARE(Json::Int64(body.value("forward_from"_L1).toObject(), "seq"_L1), photo->seq);
	QVERIFY(!body.contains("attachment"_L1));
	QCOMPARE(harness.http.count("/v1/uploads"), uploads);

	// What cannot be forwarded or queued.
	auto system = Message();
	system.kind = MessageKind::System;
	system.chat = u"c1"_s;
	system.seq = 5;
	QVERIFY(outbox.forward(u"c3"_s, system).isEmpty());
	auto pending = Message();
	pending.kind = MessageKind::Text;
	pending.chat = u"c1"_s;
	pending.text = u"not sent yet"_s;
	QVERIFY(outbox.forward(u"c3"_s, pending).isEmpty());
	QVERIFY(outbox.sendFile(u"c3"_s, u"/no/such/file.bin"_s).isEmpty());
}

void TestOutbox::echoBeforeAnswer() {
	auto harness = Harness();
	QVERIFY(harness.start());
	auto done = false;
	harness.messages().loadLatest(u"c1"_s, base::Guard(), [&] { done = true; });
	QVERIFY(Test::WaitFor([&] { return done; }));
	harness.on(u"chat.send"_s, [](const Call &) {
		return Reply::Hold();
	});
	auto hello = Rt::Frame();
	hello.type = Rt::Frame::Type::Hello;
	hello.pts = 100;
	harness.transport.setOnline(true);
	harness.transport.emitFrame(hello);

	const auto id = harness.outbox().sendText(u"c1"_s, u"echo"_s);
	Test::Settle();
	const auto before = harness.messages().history(u"c1"_s).items.size();

	// My own message comes back as an update before the RPC answers.
	auto update = Rt::Frame();
	update.type = Rt::Frame::Type::Update;
	update.pts = 101;
	update.kind = u"message"_s;
	update.data = QJsonObject{
		{ u"chat"_s, u"c1"_s },
		{ u"seq"_s, 10 },
		{ u"id"_s, u"m10"_s },
		{ u"sender"_s, u"u_me"_s },
		{ u"kind"_s, u"text"_s },
		{ u"text"_s, u"echo"_s },
		{ u"client_message_id"_s, id },
		{ u"created_at"_s, 1800000000010. },
	};
	harness.transport.emitFrame(update);
	QVERIFY(!HasLocal(harness, u"c1"_s, id));
	QCOMPARE(harness.messages().history(u"c1"_s).items.size(), before);
	QCOMPARE(harness.messages().find(u"c1"_s, 10)->clientMessageId, id);
	QVERIFY(harness.messages().find(u"c1"_s, 10)->mine);

	harness.transport.resolve(harness.transport.lastIndexOf(u"chat.send"_s), Sent(10));
	Test::Settle();
	QCOMPARE(harness.messages().history(u"c1"_s).items.size(), before);
	QVERIFY(!harness.outbox().contains(id));
	const auto row = harness.chats().chat(u"c1"_s);
	QCOMPARE(row->lastSeq, 10);
	QCOMPARE(row->lastMessage->seq, 10);
}

void TestOutbox::fileIsUploadedFirst() {
	auto dir = QTemporaryDir();
	QVERIFY(dir.isValid());
	const auto path = dir.filePath(u"report.bin"_s);
	const auto content = base::RandomBytes(10);
	{
		auto file = QFile(path);
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write(content);
	}
	auto harness = Harness();
	QVERIFY(harness.start());
	auto parts = QMap<QByteArray, QByteArray>();
	auto order = QList<QByteArray>();
	harness.http.handler = [&](const Test::HttpServer::Request &request) {
		using Response = Test::HttpServer::Response;
		order.push_back(request.method + ' ' + request.path);
		if (request.method == "POST" && request.path == "/v1/uploads") {
			const auto base = harness.http.base();
			return Response::Ok(QJsonObject{
				{ u"upload_id"_s, u"up1"_s },
				{ u"media_id"_s, u"med1"_s },
				{ u"state"_s, u"pending"_s },
				{ u"part_size"_s, 6 },
				{ u"parts"_s, QJsonArray{
					QJsonObject{ { u"number"_s, 1 }, { u"url"_s, base + u"/put/1"_s } },
					QJsonObject{ { u"number"_s, 2 }, { u"url"_s, base + u"/put/2"_s } },
				} },
			});
		} else if (request.method == "PUT") {
			parts.insert(request.path, request.body);
			return Response::Ok();
		} else if (request.path == "/v1/media/med1") {
			return Response::Ok(QJsonObject{ { u"state"_s, u"ready"_s } });
		}
		return Response::Ok();
	};
	auto seq = qint64(70);
	harness.on(u"chat.send"_s, [&](const Call &) {
		return Reply::Ok(Sent(seq++));
	});
	auto sent = QSignalSpy(&harness.outbox(), &Outbox::sent);
	auto options = SendFileOptions();
	options.kind = UploadKind::File;
	const auto id = harness.outbox().sendFile(u"c2"_s, path, options);
	QVERIFY(!id.isEmpty());
	{
		const auto &history = harness.messages().history(u"c2"_s);
		const auto index = history.indexOfKey(Message::KeyForLocal(id));
		QVERIFY(index >= 0);
		QVERIFY(history.items[index].kind == MessageKind::File);
		QCOMPARE(history.items[index].localPath, path);
		QCOMPARE(history.items[index].media->size, 10);
		QCOMPARE(history.items[index].text, u"report.bin"_s);
	}
	// A text queued behind the file waits for it.
	harness.outbox().sendText(u"c2"_s, u"after"_s);
	QVERIFY(Test::WaitFor([&] { return sent.size() == 2; }, 10000));

	const auto created = harness.http.to("/v1/uploads");
	QCOMPARE(created.size(), 1);
	QCOMPARE(created[0].json().value("purpose"_L1).toString(), u"chat"_s);
	QCOMPARE(created[0].json().value("kind"_L1).toString(), u"file"_s);
	QCOMPARE(Json::Int64(created[0].json(), "size"_L1), 10);
	QCOMPARE(created[0].json().value("filename"_L1).toString(), u"report.bin"_s);
	QCOMPARE(created[0].json().value("sha256"_L1).toString().size(), 64);
	QCOMPARE(parts.value("/put/1"), content.left(6));
	QCOMPARE(parts.value("/put/2"), content.mid(6));
	QVERIFY(order.indexOf("POST /v1/uploads/up1/complete") > order.indexOf("PUT /put/2"));
	QVERIFY(order.indexOf("POST /v1/uploads/up1/complete") > order.indexOf("PUT /put/1"));

	const auto sends = harness.bodies(u"chat.send"_s);
	QCOMPARE(sends.size(), 2);
	QCOMPARE(sends[0].value("kind"_L1).toString(), u"file"_s);
	QCOMPARE(sends[0].value("attachment"_L1).toString(), u"med1"_s);
	QCOMPARE(sends[0].value("text"_L1).toString(), u"report.bin"_s);
	QCOMPARE(sends[0].value("client_message_id"_L1).toString(), id);
	QCOMPARE(sends[1].value("text"_L1).toString(), u"after"_s);
	QCOMPARE(harness.messages().find(u"c2"_s, 70)->attachment, u"med1"_s);
}

// The `media` signal tells that processing finished: the send goes out at
// once instead of at the next poll of /v1/media/{id} (and a rejected file
// fails without more polls).
void TestOutbox::processedSignalEndsTheWait() {
	auto dir = QTemporaryDir();
	QVERIFY(dir.isValid());
	const auto write = [&](const QString &name) {
		const auto path = dir.filePath(name);
		auto file = QFile(path);
		if (file.open(QIODevice::WriteOnly)) {
			file.write(base::RandomBytes(10));
		}
		return path;
	};
	auto harness = Harness();
	QVERIFY(harness.start());
	auto uploads = 0;
	auto polls = QMap<QByteArray, int>();
	harness.http.handler = [&](const Test::HttpServer::Request &request) {
		using Response = Test::HttpServer::Response;
		if (request.method == "POST" && request.path == "/v1/uploads") {
			++uploads;
			return Response::Ok(QJsonObject{
				{ u"upload_id"_s, u"up%1"_s.arg(uploads) },
				{ u"media_id"_s, u"med%1"_s.arg(uploads) },
				{ u"state"_s, u"pending"_s },
				{ u"part_size"_s, 10 },
				{ u"parts"_s, QJsonArray{
					QJsonObject{ { u"number"_s, 1 }, { u"url"_s, harness.http.base() + u"/put/%1"_s.arg(uploads) } },
				} },
			});
		} else if (request.path.startsWith("/v1/media/")) {
			++polls[request.path];
			return Response::Ok(QJsonObject{ { u"state"_s, u"processing"_s } });
		}
		return Response::Ok();
	};
	harness.on(u"chat.send"_s, [&](const Call &) {
		return Reply::Ok(Sent(80));
	});
	const auto processed = [&](const QString &media, const QString &state) {
		auto frame = Rt::Frame();
		frame.type = Rt::Frame::Type::Media;
		frame.data = QJsonObject{ { u"media"_s, media }, { u"state"_s, state } };
		harness.transport.emitFrame(frame);
	};
	auto sent = QSignalSpy(&harness.outbox(), &Outbox::sent);
	auto options = SendFileOptions();
	options.kind = UploadKind::File;
	harness.outbox().sendFile(u"c2"_s, write(u"a.bin"_s), options);
	QVERIFY(Test::WaitFor([&] { return polls.value("/v1/media/med1") > 0; }, 10000));
	processed(u"med1"_s, u"ready"_s);
	QVERIFY(Test::WaitFor([&] { return sent.size() == 1; }, 3000));
	QCOMPARE(harness.bodies(u"chat.send"_s).back().value("attachment"_L1).toString(), u"med1"_s);
	QVERIFY(polls.value("/v1/media/med1") <= 2);

	// Rejected: the message fails, nothing is sent.
	auto failed = QSignalSpy(&harness.outbox(), &Outbox::failed);
	harness.outbox().sendFile(u"c2"_s, write(u"b.bin"_s), options);
	QVERIFY(Test::WaitFor([&] { return polls.value("/v1/media/med2") > 0; }, 10000));
	processed(u"med2"_s, u"rejected"_s);
	QVERIFY(Test::WaitFor([&] { return !failed.isEmpty(); }, 3000));
	QCOMPARE(sent.size(), 1);
	QCOMPARE(harness.bodies(u"chat.send"_s).size(), 1);
}

void TestOutbox::queueSurvivesRestart() {
	auto dir = QTemporaryDir();
	QVERIFY(dir.isValid());
	const auto path = dir.filePath(u"cache.db"_s);
	auto first = QString();
	auto second = QString();
	auto refused = QString();
	{
		auto harness = Harness(path);
		QVERIFY(harness.start());
		harness.on(u"chat.send"_s, [](const Call &call) {
			return (call.body.value("text"_L1).toString() == u"refused")
				? Reply::Fail(422, u"message.invalid"_s)
				: Reply::Hold();
		});
		refused = harness.outbox().sendText(u"c2"_s, u"refused"_s);
		first = harness.outbox().sendText(u"c1"_s, u"first"_s);
		second = harness.outbox().sendText(u"c1"_s, u"second"_s);
		QVERIFY(Test::WaitFor([&] {
			return StatusOf(harness, u"c2"_s, refused) == MessageStatus::Failed;
		}));
		// The application quits with the answers still out.
	}
	{
		auto harness = Harness(path);
		auto seq = qint64(80);
		harness.on(u"chat.send"_s, [&](const Call &) {
			return Reply::Ok(Sent(seq++));
		});
		auto ready = false;
		harness.session->start([&] {
			// The cached state is in the stores: the bubbles are back.
			ready = true;
			QCOMPARE(harness.outbox().items().size(), 3);
			QVERIFY(HasLocal(harness, u"c1"_s, first));
			QVERIFY(HasLocal(harness, u"c1"_s, second));
			QVERIFY(StatusOf(harness, u"c2"_s, refused) == MessageStatus::Failed);
		});
		QVERIFY(Test::WaitFor([&] { return ready && harness.outbox().pendingCount() == 0; }));
		const auto sends = harness.bodies(u"chat.send"_s);
		QCOMPARE(sends.size(), 2);
		QCOMPARE(sends[0].value("client_message_id"_L1).toString(), first);
		QCOMPARE(sends[0].value("text"_L1).toString(), u"first"_s);
		QCOMPARE(sends[1].value("client_message_id"_L1).toString(), second);
		// The failed one is still there for the user to retry or discard.
		const auto items = harness.outbox().items();
		QCOMPARE(items.size(), 1);
		QCOMPARE(items[0].clientMessageId, refused);
		QCOMPARE(items[0].errorCode, u"message.invalid"_s);
		harness.outbox().discard(refused);
	}
	{
		auto harness = Harness(path);
		auto ready = false;
		harness.session->start([&] { ready = true; });
		QVERIFY(Test::WaitFor([&] { return ready; }));
		QVERIFY(harness.outbox().items().isEmpty());
		QCOMPARE(harness.transport.count(u"chat.send"_s), 0);
	}
}

QTEST_GUILESS_MAIN(TestOutbox)
#include "test_outbox.moc"
