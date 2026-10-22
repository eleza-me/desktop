// Data::Chats on a fake transport: which rows the list shows, what an inbox
// reload keeps, and how unread counts follow read marks.
#include "data/data_json.h"
#include "test_harness.h"

#include <QtTest/QSignalSpy>

using namespace Data;
using Test::Harness;
using Reply = Test::FakeTransport::Reply;
using Call = Test::FakeTransport::Call;
using Type = Rt::Frame::Type;
using Section = Chats::Section;

namespace {

[[nodiscard]] Rt::Frame Hello(qint64 pts) {
	auto frame = Rt::Frame();
	frame.type = Type::Hello;
	frame.pts = pts;
	return frame;
}

[[nodiscard]] Rt::Frame Update(qint64 pts, const QString &kind, const QJsonObject &data) {
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

// The header `chat.thread` answers for a thread nobody wrote in yet.
[[nodiscard]] QJsonObject EmptyThread(const QString &chat, const QString &peer) {
	return QJsonObject{
		{ u"chat"_s, chat },
		{ u"kind"_s, u"direct"_s },
		{ u"state"_s, u"active"_s },
		{ u"participants"_s, QJsonArray{
			QJsonObject{ { u"ref"_s, u"u_me"_s }, { u"user_id"_s, u"u_me"_s }, { u"me"_s, true } },
			QJsonObject{ { u"ref"_s, peer }, { u"user_id"_s, peer } },
		} },
		{ u"last_seq"_s, 0 },
		{ u"can_send"_s, true },
	};
}

// The fixture's message section without the chats in `drop`.
[[nodiscard]] QJsonObject InboxWithout(const QStringList &drop) {
	auto result = Test::Fixture("inbox_message.json");
	auto chats = QJsonArray();
	for (const auto &entry : result.value("chats"_L1).toArray()) {
		if (!drop.contains(entry.toObject().value("chat"_L1).toString())) {
			chats.push_back(entry);
		}
	}
	result.insert(u"chats"_s, chats);
	return result;
}

[[nodiscard]] QStringList Ids(const QVector<Chat> &list) {
	auto result = QStringList();
	for (const auto &chat : list) {
		result.push_back(chat.id);
	}
	return result;
}

[[nodiscard]] bool Open(Harness &harness, const QString &user) {
	auto opened = false;
	harness.chats().openDirect(user, base::Guard(), [&](const Chat &) {
		opened = true;
	});
	return Test::WaitFor([&] { return opened; });
}

[[nodiscard]] bool Reload(Harness &harness) {
	auto done = false;
	harness.chats().loadInbox(base::Guard(), [&] { done = true; });
	return Test::WaitFor([&] { return done; });
}

[[nodiscard]] Message Item(qint64 seq, bool mine) {
	auto result = Message();
	result.chat = u"c1"_s;
	result.seq = seq;
	result.id = u"m%1"_s.arg(seq);
	result.sender = mine ? u"u_me"_s : u"u_sara"_s;
	result.mine = mine;
	result.kind = MessageKind::Text;
	result.text = u"x"_s;
	return result;
}

} // namespace

class TestChats final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void contentRule();
	void openedEmptyChatIsNotListed();
	void inboxReloadRemovesOnlyWhatIsGone();
	void truncatedInboxKeepsOlderRows();
	void inboxOnItsWayKeepsWhatIsNewer();
	void unreadAfterRead();
	void partialAndRemoteReads();
	void scrolledUpCountsUnread();

};

void TestChats::contentRule() {
	auto chat = Chat();
	chat.id = u"c9"_s;
	chat.section = u"message"_s;
	QVERIFY(!Chats::HasContent(chat));
	chat.pinned = true;
	QVERIFY(Chats::HasContent(chat));
	chat.pinned = false;
	chat.draft = Draft();
	QVERIFY(!Chats::HasContent(chat)); // an empty draft is no draft
	chat.draft->text = u"سلام"_s;
	QVERIFY(Chats::HasContent(chat));
	chat.draft = std::nullopt;
	chat.lastSeq = 3;
	QVERIFY(Chats::HasContent(chat));
	chat.lastSeq = 0;
	chat.lastMessage = Item(-1, true); // a message waiting in the outbox
	QVERIFY(Chats::HasContent(chat));
	chat.lastMessage = std::nullopt;
	// Conversations and groups are listed from their start.
	chat.isConversation = true;
	QVERIFY(Chats::HasContent(chat));
	chat.isConversation = false;
	chat.isGroup = true;
	QVERIFY(Chats::HasContent(chat));
}

void TestChats::openedEmptyChatIsNotListed() {
	auto harness = Harness();
	QVERIFY(harness.start());
	auto &chats = harness.chats();
	harness.on(u"chat.thread"_s, [](const Call &) {
		return Reply::Ok(EmptyThread(u"c9"_s, u"u_ali"_s));
	});
	const auto all = Ids(chats.list(Section::All));
	const auto messages = chats.count(Section::Messages);
	const auto badge = chats.unreadChats();

	// Opened from a profile / a search result: a row for the screen, not
	// one in the list.
	QVERIFY(Open(harness, u"u_ali"_s));
	QVERIFY(chats.chat(u"c9"_s) != nullptr);
	QVERIFY(!chats.listed(*chats.chat(u"c9"_s)));
	QCOMPARE(Ids(chats.list(Section::All)), all);
	QCOMPARE(chats.count(Section::Messages), messages);
	QCOMPARE(chats.unreadChats(), badge);
	QVERIFY(chats.all().size() > all.size()); // the store has it

	// A draft lists it, clearing the draft takes it out again.
	auto changes = QSignalSpy(&chats, &Chats::listChanged);
	auto draft = Draft();
	draft.text = u"سلام"_s;
	chats.saveDraft(u"c9"_s, draft);
	QVERIFY(Ids(chats.list(Section::All)).contains(u"c9"_s));
	QVERIFY(!changes.isEmpty());
	changes.clear();
	chats.clearDraft(u"c9"_s);
	QVERIFY(!Ids(chats.list(Section::All)).contains(u"c9"_s));
	QVERIFY(!changes.isEmpty());

	// The server's inbox omits the empty thread: the open chat stays (no
	// chatRemoved, so its screen does not close).
	chats.setVisibleChat(u"c9"_s);
	auto removed = QSignalSpy(&chats, &Chats::chatRemoved);
	QVERIFY(Reload(harness));
	QVERIFY(chats.chat(u"c9"_s) != nullptr);
	QVERIFY(removed.isEmpty());
	QCOMPARE(Ids(chats.list(Section::All)), all);

	// The first message: it is a chat of the list like any other.
	harness.transport.setOnline(true);
	harness.transport.emitFrame(Hello(100));
	changes.clear();
	harness.transport.emitFrame(Update(101, u"message"_s, MessageJson(u"c9"_s, 1, u"u_ali"_s, u"سلام!"_s)));
	QVERIFY(chats.listed(*chats.chat(u"c9"_s)));
	QVERIFY(Ids(chats.list(Section::Messages)).contains(u"c9"_s));
	QCOMPARE(chats.count(Section::Messages), messages + 1);
	QVERIFY(!changes.isEmpty());
}

void TestChats::inboxReloadRemovesOnlyWhatIsGone() {
	auto harness = Harness();
	QVERIFY(harness.start());
	auto &chats = harness.chats();
	harness.on(u"chat.thread"_s, [](const Call &) {
		return Reply::Ok(EmptyThread(u"c9"_s, u"u_ali"_s));
	});
	QVERIFY(Open(harness, u"u_ali"_s));
	// A message for c3 that cannot go out yet stays in the outbox.
	harness.on(u"chat.send"_s, [](const Call &) {
		return Reply::Fail(503, u"server.unavailable"_s);
	});
	harness.outbox().sendText(u"c3"_s, u"بعدا"_s);
	QVERIFY(Test::WaitFor([&] { return harness.transport.count(u"chat.send"_s) > 0; }));
	QVERIFY(harness.outbox().holds(u"c3"_s));

	// The server now lists only c1.
	harness.on(u"chat.inbox"_s, [](const Call &call) {
		return Reply::Ok((call.body.value("section"_L1).toString() == u"message")
			? InboxWithout({ u"c2"_s, u"c3"_s })
			: QJsonObject{ { u"chats"_s, QJsonArray() } });
	});
	auto removed = QSignalSpy(&chats, &Chats::chatRemoved);
	QVERIFY(Reload(harness));
	QCOMPARE(removed.size(), 1);
	QCOMPARE(removed[0][0].toString(), u"c2"_s);
	QVERIFY(chats.chat(u"c2"_s) == nullptr);
	QVERIFY(chats.chat(u"c3"_s) != nullptr); // waiting outbox message
	QVERIFY(chats.listed(*chats.chat(u"c3"_s)));
	QVERIFY(chats.chat(u"c9"_s) != nullptr); // empty, not listed anyway
	QVERIFY(!chats.listed(*chats.chat(u"c9"_s)));
}

void TestChats::truncatedInboxKeepsOlderRows() {
	auto harness = Harness();
	// A request row in the first inbox.
	const auto request = [] {
		auto entry = Test::Fixture("inbox_message.json")
			.value("chats"_L1).toArray().at(2).toObject();
		entry.insert(u"chat"_s, u"c5"_s);
		entry.insert(u"section"_s, u"request"_s);
		auto info = entry.value("chat_info"_L1).toObject();
		info.insert(u"chat"_s, u"c5"_s);
		info.insert(u"request"_s, u"incoming"_s);
		entry.insert(u"chat_info"_s, info);
		auto last = entry.value("last_message"_L1).toObject();
		last.insert(u"chat"_s, u"c5"_s);
		entry.insert(u"last_message"_s, last);
		return entry;
	}();
	harness.on(u"chat.inbox"_s, [=](const Call &call) {
		const auto section = call.body.value("section"_L1).toString();
		return Reply::Ok((section == u"message")
			? Test::Fixture("inbox_message.json")
			: (section == u"request")
			? QJsonObject{ { u"chats"_s, QJsonArray{ request } } }
			: QJsonObject{ { u"chats"_s, QJsonArray() } });
	});
	QVERIFY(harness.start());
	auto &chats = harness.chats();
	QVERIFY(Test::WaitFor([&] { return chats.chat(u"c5"_s) != nullptr; }));
	QVERIFY(chats.chat(u"c5"_s)->isRequest());

	// Every page names a next one (more chats than the client reads), and
	// the requests section fails: neither is known whole now.
	auto pages = 0;
	harness.on(u"chat.inbox"_s, [&](const Call &call) {
		const auto section = call.body.value("section"_L1).toString();
		if (section == u"request") {
			return Reply::Fail(503, u"server.unavailable"_s);
		} else if (section != u"message") {
			return Reply::Ok(QJsonObject{ { u"chats"_s, QJsonArray() } });
		}
		auto page = InboxWithout({ u"c2"_s, u"c3"_s });
		page.insert(u"next_cursor"_s, u"p%1"_s.arg(++pages));
		return Reply::Ok(page);
	});
	auto removed = QSignalSpy(&chats, &Chats::chatRemoved);
	QVERIFY(Reload(harness));
	QVERIFY(pages > 1);
	QVERIFY(removed.isEmpty());
	QVERIFY(chats.chat(u"c2"_s) != nullptr);
	QVERIFY(chats.chat(u"c3"_s) != nullptr);
	QVERIFY(chats.chat(u"c5"_s) != nullptr);

	// Read whole again: what the server no longer has goes.
	harness.on(u"chat.inbox"_s, [](const Call &call) {
		return Reply::Ok((call.body.value("section"_L1).toString() == u"message")
			? InboxWithout({ u"c2"_s })
			: QJsonObject{ { u"chats"_s, QJsonArray() } });
	});
	QVERIFY(Reload(harness));
	QVERIFY(chats.chat(u"c2"_s) == nullptr);
	QVERIFY(chats.chat(u"c5"_s) == nullptr);
	QCOMPARE(removed.size(), 2);
}

// The inbox is how the rows were when the server read them: messages and
// read marks applied while it was on its way are newer and stay.
void TestChats::inboxOnItsWayKeepsWhatIsNewer() {
	auto harness = Harness();
	QVERIFY(harness.start());
	harness.transport.setOnline(true);
	harness.transport.emitFrame(Hello(100));
	auto &chats = harness.chats();
	QCOMPARE(chats.chat(u"c1"_s)->lastSeq, 9);
	QCOMPARE(chats.chat(u"c1"_s)->unread, 2);
	auto held = -1;
	harness.on(u"chat.inbox"_s, [&](const Call &call) {
		if (call.body.value("section"_L1).toString() != u"message") {
			return Reply::Ok(QJsonObject{ { u"chats"_s, QJsonArray() } });
		}
		held = int(harness.transport.calls.size()) - 1;
		return Reply::Hold();
	});
	const auto reloadHeld = [&](Fn<void()> meanwhile) {
		auto done = false;
		held = -1;
		chats.loadInbox(base::Guard(), [&] { done = true; });
		if (held < 0) {
			return false;
		}
		meanwhile();
		harness.transport.resolve(held, Test::Fixture("inbox_message.json"));
		return Test::WaitFor([&] { return done; });
	};

	// Two messages, one of them mine, while the inbox is on its way.
	auto pts = qint64(100);
	QVERIFY(reloadHeld([&] {
		harness.transport.emitFrame(Update(++pts, u"message"_s, MessageJson(u"c1"_s, 10, u"u_sara"_s, u"تازه"_s)));
		harness.transport.emitFrame(Update(++pts, u"message"_s, MessageJson(u"c1"_s, 11, u"u_me"_s, u"من"_s)));
		QCOMPARE(chats.chat(u"c1"_s)->unread, 3);
	}));
	auto row = chats.chat(u"c1"_s);
	QCOMPARE(row->lastSeq, 11);
	QVERIFY(row->lastMessage.has_value());
	QCOMPARE(row->lastMessage->seq, 11);
	QVERIFY(row->lastMine);
	QCOMPARE(row->unread, 3);
	// A row nothing happened to is as the inbox says.
	QCOMPARE(chats.chat(u"c2"_s)->lastSeq, 4);

	// Read here while the next one is on its way: the badge does not come back.
	QVERIFY(reloadHeld([&] {
		harness.messages().markRead(u"c1"_s, 11);
		QCOMPARE(chats.chat(u"c1"_s)->unread, 0);
	}));
	row = chats.chat(u"c1"_s);
	QCOMPARE(row->lastSeq, 11);
	QCOMPARE(row->lastReadSeq, 11);
	QCOMPARE(row->unread, 0);
}

void TestChats::unreadAfterRead() {
	// c1: 1..9, mine are 2, 4 and 8.
	auto history = History();
	history.chat = u"c1"_s;
	history.loaded = true;
	history.hasMoreBefore = false;
	for (auto seq = 1; seq <= 9; ++seq) {
		history.items.push_back(Item(seq, seq == 2 || seq == 4 || seq == 8));
	}
	auto row = Chat();
	row.id = u"c1"_s;
	row.lastSeq = 9;
	row.unread = 3;
	row.lastReadSeq = 5;

	// Read to the end: nothing left.
	QCOMPARE(UnreadAfterRead(row, &history, 9).count, 0);
	QCOMPARE(UnreadAfterRead(row, &history, 12).count, 0);
	// Read to the middle: what is below and not mine.
	QCOMPARE(UnreadAfterRead(row, &history, 5).count, 3); // 6, 7, 9
	QCOMPARE(UnreadAfterRead(row, &history, 7).count, 1); // 9
	QCOMPARE(UnreadAfterRead(row, &history, 8).count, 1); // 9
	// Never more than the row had.
	row.unread = 2;
	QCOMPARE(UnreadAfterRead(row, &history, 5).count, 2);
	row.unread = 0;
	QCOMPARE(UnreadAfterRead(row, &history, 5).count, 0);

	// Without the messages: the seqs above the mark bound it.
	row.unread = 3;
	QCOMPARE(UnreadAfterRead(row, nullptr, 7).count, 2);
	QCOMPARE(UnreadAfterRead(row, nullptr, 8).count, 1);
	QCOMPARE(UnreadAfterRead(row, nullptr, 3).count, 3);
	// A window that does not reach down to the mark counts like that too.
	auto window = History();
	window.loaded = true;
	window.hasMoreBefore = true;
	window.items = { Item(8, true), Item(9, false) };
	QCOMPARE(UnreadAfterRead(row, &window, 5).count, 3);
	QCOMPARE(UnreadAfterRead(row, &window, 7).count, 1); // it does reach 8
	// A detached window (not at the end) is not counted in.
	window.hasMoreAfter = true;
	QCOMPARE(UnreadAfterRead(row, &window, 7).count, 2);

	// A capped count ("99+") stays a lower bound while it can be one.
	row.lastSeq = 1000;
	row.unread = 99;
	row.unreadCapped = true;
	const auto far = UnreadAfterRead(row, nullptr, 10);
	QCOMPARE(far.count, 99);
	QVERIFY(far.capped);
	const auto near = UnreadAfterRead(row, nullptr, 990);
	QCOMPARE(near.count, 10);
	QVERIFY(!near.capped);
}

void TestChats::partialAndRemoteReads() {
	auto harness = Harness();
	QVERIFY(harness.start());
	auto loaded = false;
	harness.messages().loadLatest(u"c1"_s, base::Guard(), [&] { loaded = true; });
	QVERIFY(Test::WaitFor([&] { return loaded; }));
	auto &chats = harness.chats();
	QCOMPARE(chats.chat(u"c1"_s)->unread, 2);
	harness.transport.setOnline(true);
	harness.transport.emitFrame(Hello(100));

	// Read on another device up to 7: only 9 is left.
	harness.transport.emitFrame(Update(101, u"read"_s, QJsonObject{
		{ u"chat"_s, u"c1"_s },
		{ u"seq"_s, 7 },
		{ u"reader"_s, u"u_me"_s },
	}));
	QCOMPARE(chats.chat(u"c1"_s)->lastReadSeq, 7);
	QCOMPARE(chats.chat(u"c1"_s)->unread, 1);

	// Two new messages, then read here to the first of them.
	harness.transport.emitFrame(Update(102, u"message"_s, MessageJson(u"c1"_s, 10, u"u_sara"_s, u"a"_s)));
	harness.transport.emitFrame(Update(103, u"message"_s, MessageJson(u"c1"_s, 11, u"u_sara"_s, u"b"_s)));
	QCOMPARE(chats.chat(u"c1"_s)->unread, 3);
	harness.messages().markRead(u"c1"_s, 10);
	QCOMPARE(chats.chat(u"c1"_s)->unread, 1);
	QVERIFY(chats.unreadChats() > 0);
	harness.messages().markRead(u"c1"_s, 11);
	QCOMPARE(chats.chat(u"c1"_s)->unread, 0);
	QCOMPARE(harness.bodies(u"chat.read"_s).size(), 2);
}

void TestChats::scrolledUpCountsUnread() {
	auto harness = Harness();
	QVERIFY(harness.start());
	auto &chats = harness.chats();
	harness.transport.setOnline(true);
	harness.transport.emitFrame(Hello(100));
	const auto unread = chats.chat(u"c2"_s)->unread;

	// On screen with the newest message in view: read as it comes.
	chats.setVisibleChat(u"c2"_s, true);
	QVERIFY(chats.readingLive(u"c2"_s));
	harness.transport.emitFrame(Update(101, u"message"_s, MessageJson(u"c2"_s, 5, u"u_ali"_s, u"a"_s)));
	QCOMPARE(chats.chat(u"c2"_s)->unread, unread);

	// Scrolled up: it counts until it is scrolled into view.
	chats.setVisibleChat(u"c2"_s, false);
	QVERIFY(!chats.readingLive(u"c2"_s));
	QCOMPARE(chats.visibleChat(), u"c2"_s);
	harness.transport.emitFrame(Update(102, u"message"_s, MessageJson(u"c2"_s, 6, u"u_ali"_s, u"b"_s)));
	QCOMPARE(chats.chat(u"c2"_s)->unread, unread + 1);

	// Gone from the screen: «at the end» means nothing then.
	chats.setVisibleChat(QString(), false);
	QVERIFY(!chats.readingLive(QString()));
}

QTEST_GUILESS_MAIN(TestChats)
#include "test_chats.moc"
