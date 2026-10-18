// Data::Groups on a fake transport: the cold start and the catch-up call,
// the pts rule for live frames, the per-group difference, sends through the
// outbox, my own place in a group (the user update of kind `group`) and the
// errors that change state.
#include "api/api_client_config.h"
#include "data/data_group_rules.h"
#include "data/data_groups.h"
#include "data/data_json.h"
#include "test_harness.h"

#include <QtCore/QTemporaryDir>
#include <QtTest/QSignalSpy>

using namespace Data;
using Test::Harness;
using Reply = Test::FakeTransport::Reply;
using Call = Test::FakeTransport::Call;
using Type = Rt::Frame::Type;

namespace {

const auto kGroup = u"j1group"_s;
const auto kOther = u"j1other"_s;

[[nodiscard]] QJsonObject Header(
		const QString &id,
		qint64 pts,
		qint64 lastSeq,
		QJsonObject extra = {}) {
	auto result = QJsonObject{
		{ u"group"_s, id },
		{ u"title"_s, u"هم‌کلاسی‌ها"_s },
		{ u"member_count"_s, 3 },
		{ u"state"_s, u"active"_s },
		{ u"status"_s, u"member"_s },
		{ u"role"_s, u"member"_s },
		{ u"rights"_s, QJsonArray() },
		{ u"permissions"_s, QJsonArray{ u"send_messages"_s, u"send_media"_s, u"send_links"_s, u"add_members"_s } },
		{ u"default_permissions"_s, QJsonArray{ u"send_messages"_s, u"send_media"_s, u"send_links"_s, u"add_members"_s } },
		{ u"can_send"_s, true },
		{ u"history_visible"_s, true },
		{ u"last_seq"_s, double(lastSeq) },
		{ u"pts"_s, double(pts) },
		{ u"last_read_seq"_s, double(lastSeq) },
		{ u"unread"_s, 0 },
		{ u"notify"_s, u"all"_s },
		{ u"pins"_s, QJsonArray() },
		{ u"created_at"_s, 1791724910041. },
	};
	for (auto i = extra.begin(); i != extra.end(); ++i) {
		result.insert(i.key(), i.value());
	}
	return result;
}

[[nodiscard]] QJsonObject MessageJson(
		const QString &group,
		qint64 seq,
		const QString &sender,
		const QString &text,
		QJsonObject extra = {}) {
	auto result = QJsonObject{
		{ u"chat"_s, group },
		{ u"seq"_s, double(seq) },
		{ u"id"_s, u"m_%1_%2"_s.arg(group).arg(seq) },
		{ u"sender"_s, sender },
		{ u"kind"_s, u"text"_s },
		{ u"text"_s, text },
		{ u"created_at"_s, 1791724910000. + double(seq) * 1000. },
	};
	for (auto i = extra.begin(); i != extra.end(); ++i) {
		result.insert(i.key(), i.value());
	}
	return result;
}

[[nodiscard]] QJsonObject Note(const QString &group, qint64 seq, const QString &sender, const QString &system) {
	auto result = MessageJson(group, seq, sender, QString());
	result.insert(u"kind"_s, u"system"_s);
	result.insert(u"system"_s, system);
	result.remove(u"text"_s);
	return result;
}

[[nodiscard]] QJsonObject Entry(const QJsonObject &header, const QJsonObject &last = {}) {
	auto result = QJsonObject{ { u"group"_s, header } };
	if (!last.isEmpty()) {
		result.insert(u"last_message"_s, last);
		result.insert(u"last_activity_at"_s, last.value("created_at"_L1));
	}
	return result;
}

[[nodiscard]] Rt::Frame GroupFrame(
		const QString &group,
		qint64 pts,
		const QString &kind,
		const QJsonValue &data = QJsonValue()) {
	auto body = QJsonObject{
		{ u"t"_s, u"group"_s },
		{ u"group"_s, group },
		{ u"pts"_s, double(pts) },
		{ u"kind"_s, kind },
	};
	if (data.isObject()) {
		body.insert(u"data"_s, data);
	}
	auto frame = Rt::Frame();
	frame.type = Type::Group;
	frame.kind = kind;
	frame.chat = group;
	frame.pts = pts;
	frame.data = body;
	return frame;
}

// A user update of kind `group` as the connection delivers it.
[[nodiscard]] Rt::Frame UserUpdate(qint64 pts, const QJsonObject &data) {
	auto frame = Rt::Frame();
	frame.type = Type::Update;
	frame.pts = pts;
	frame.kind = u"group"_s;
	frame.data = data;
	return frame;
}

[[nodiscard]] Rt::Frame Hello(qint64 pts) {
	auto frame = Rt::Frame();
	frame.type = Type::Hello;
	frame.pts = pts;
	return frame;
}

// A client-config that switches groups on, a session on a fake transport.
struct Fixture {
	explicit Fixture(bool groups = true, bool administration = true, const QString &database = QString()) {
		configServer.handler = [=](const Test::HttpServer::Request &) {
			auto values = QJsonObject();
			if (groups) {
				values.insert(u"feature.groups"_s, true);
			}
			if (administration) {
				values.insert(u"groups.max_admins"_s, 50);
			}
			// The server's own shape: the keys live under "values".
			return Test::HttpServer::Response::Ok(QJsonObject{
				{ u"version"_s, 7 },
				{ u"values"_s, values },
			});
		};
		configApi = std::make_unique<Api::Client>(configServer.base(), u"1.0.0"_s);
		config = std::make_unique<Api::ClientConfig>(configApi.get(), nullptr);
		auto loaded = false;
		config->load(nullptr, [&](bool) { loaded = true; });
		const auto ok = Test::WaitFor([&] { return loaded; });
		Q_ASSERT(ok);
		Q_UNUSED(ok);
		auto options = Harness::Options();
		options.clientConfig = config.get();
		options.databasePath = database;
		harness = std::make_unique<Harness>(options);
	}

	// One group in the list (pts 6, last message seq 4), the session started.
	[[nodiscard]] bool startWithGroup(int unread = 0) {
		harness->on(u"groups.list"_s, [=](const Call &) {
			return Reply::Ok(QJsonObject{ { u"groups"_s, QJsonArray{
				Entry(
					Header(kGroup, 6, 4, { { u"unread"_s, unread }, { u"last_read_seq"_s, double(4 - unread) } }),
					MessageJson(kGroup, 4, u"u_sara"_s, u"سلام به همه"_s)),
			} } });
		});
		return harness->start()
			&& Test::WaitFor([&] { return groups().pts(kGroup) == 6; });
	}

	// Loads the group's newest page (messages 1..lastSeq at `pts`).
	[[nodiscard]] bool open(qint64 pts = 6, qint64 lastSeq = 4) {
		harness->on(u"groups.history"_s, [=](const Call &) {
			auto list = QJsonArray();
			for (auto seq = qint64(1); seq <= lastSeq; ++seq) {
				list.push_back(MessageJson(kGroup, seq, (seq % 2) ? u"u_sara"_s : u"u_ali"_s, u"m%1"_s.arg(seq)));
			}
			return Reply::Ok(QJsonObject{
				{ u"group"_s, Header(kGroup, pts, lastSeq) },
				{ u"messages"_s, list },
				{ u"has_more_before"_s, false },
			});
		});
		auto done = false;
		harness->messages().loadLatest(kGroup, base::Guard(), [&] { done = true; });
		return Test::WaitFor([&] { return done; });
	}

	[[nodiscard]] Groups &groups() const {
		return harness->session->groups();
	}
	[[nodiscard]] const Chat *row(const QString &id = kGroup) const {
		return harness->chats().chat(id);
	}
	void frame(const Rt::Frame &frame) {
		harness->transport.emitFrame(frame);
	}
	[[nodiscard]] int count(const QString &method) const {
		return harness->transport.count(method);
	}
	[[nodiscard]] QStringList timeline() const {
		auto result = QStringList();
		for (const auto &message : harness->messages().history(kGroup).items) {
			result.push_back(u"%1=%2"_s.arg(message.seq).arg(message.text));
		}
		return result;
	}

	Test::HttpServer configServer;
	std::unique_ptr<Api::Client> configApi;
	std::unique_ptr<Api::ClientConfig> config;
	std::unique_ptr<Harness> harness;
};

} // namespace

class TestGroups final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void offWithoutTheFlag();
	void gatesFollowClientConfig();
	void coldStartPagesTheList();
	void groupsSurviveTheInbox();
	void catchUpSendsWhatIsHeld();
	void nextFrameIsApplied();
	void oldFrameIsDropped();
	void gapWithoutCacheAsksTheCatchUp();
	void unknownGroupAsksTheCatchUpOnce();
	void openAttachesTheCache();
	void gapWithCacheReadsTheDifference();
	void framesWaitForTheDifference();
	void tooLongStartsOver();
	void editsDeletesAndReactions();
	void notesMoveTheHeader();
	void mentionsAreCounted();
	void sendGoesToTheGroupRoute();
	void sendAnswerThenFrame();
	void frameThenSendAnswer();
	void slowModeKeepsTheMessageQueued();
	void ownPlaceMembership();
	void ownPlaceReadSettingsReaction();
	void notFoundDropsTheGroup();
	void purgedFrameDropsTheGroup();
	void actionsUseTheGroupRoutes();
	void permissionsGoOverHttp();
	void cachedGroupIsRepairedOnOpen();
	void typingAndAdminSignals();

};

void TestGroups::offWithoutTheFlag() {
	auto fixture = Fixture(false, false);
	QVERIFY(fixture.harness->start());
	Test::Settle(50);
	QVERIFY(!fixture.groups().enabled());
	fixture.harness->transport.setOnline(true);
	fixture.frame(Hello(100));
	fixture.frame(GroupFrame(kGroup, 2, u"message"_s, MessageJson(kGroup, 2, u"u_sara"_s, u"hi"_s)));
	fixture.frame(UserUpdate(101, QJsonObject{ { u"group"_s, kGroup }, { u"status"_s, u"member"_s } }));
	Test::Settle(50);
	for (const auto &method : fixture.harness->transport.methods()) {
		QVERIFY2(!method.startsWith(u"groups."), qPrintable(method));
	}
	QVERIFY(!fixture.row());
}

void TestGroups::gatesFollowClientConfig() {
	{
		auto fixture = Fixture(true, false);
		QVERIFY(fixture.groups().enabled());
		QVERIFY(!fixture.groups().administration());
	}
	auto fixture = Fixture(true, true);
	QVERIFY(fixture.groups().enabled());
	QVERIFY(fixture.groups().administration());
	QCOMPARE(fixture.groups().maxAdmins(), 50);
	QCOMPARE(fixture.groups().maxMembers(), 5000); // not listed: the default
	QCOMPARE(fixture.groups().inviteLinkBase(), u"eleza.me/g/"_s);
}

void TestGroups::coldStartPagesTheList() {
	auto fixture = Fixture();
	fixture.harness->on(u"groups.list"_s, [](const Call &call) {
		if (call.body.value("cursor"_L1).toString().isEmpty()) {
			return Reply::Ok(QJsonObject{
				{ u"groups"_s, QJsonArray{
					Entry(
						Header(kGroup, 6, 4, { { u"unread"_s, 2 }, { u"unread_mentions"_s, 1 }, { u"pinned"_s, true } }),
						MessageJson(kGroup, 4, u"u_sara"_s, u"سلام به همه"_s)),
				} },
				{ u"next_cursor"_s, u"page2"_s },
			});
		}
		return Reply::Ok(QJsonObject{ { u"groups"_s, QJsonArray{
			Entry(Header(kOther, 1, 1), Note(kOther, 1, u"u_me"_s, u"group_created"_s)),
		} } });
	});
	QVERIFY(fixture.harness->start());
	QVERIFY(Test::WaitFor([&] { return fixture.row(kOther) != nullptr; }));
	QCOMPARE(fixture.count(u"groups.list"_s), 2);
	QCOMPARE(fixture.count(u"groups.sync"_s), 0);

	const auto row = fixture.row();
	QVERIFY(row && row->isGroup && row->group);
	QCOMPARE(row->title, u"هم‌کلاسی‌ها"_s);
	QCOMPARE(row->section, u"message"_s);
	QCOMPARE(row->groupPts, qint64(6));
	QCOMPARE(row->groupCachePts, qint64(-1)); // no history was read
	QCOMPARE(row->lastSeq, qint64(4));
	QCOMPARE(row->unread, 2);
	QCOMPARE(row->unreadMentions, 1);
	QVERIFY(row->pinned);
	QVERIFY(row->lastMessage && row->lastMessage->text == u"سلام به همه");
	QVERIFY(!row->lastMine);
	QVERIFY(fixture.row(kOther)->lastMine);
	// The rows are in the Messages section with the direct chats.
	auto ids = QStringList();
	for (const auto &chat : fixture.harness->chats().list()) {
		ids.push_back(chat.id);
	}
	QVERIFY(ids.contains(kGroup) && ids.contains(kOther) && ids.contains(u"c1"_s));
	QCOMPARE(ids.front(), kGroup); // pinned first
	QCOMPARE(fixture.harness->chats().unreadChats() >= 1, true);
}

void TestGroups::groupsSurviveTheInbox() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	// The inbox does not list groups: reading it again keeps their rows.
	auto done = false;
	fixture.harness->chats().loadInbox(base::Guard(), [&] { done = true; });
	QVERIFY(Test::WaitFor([&] { return done; }));
	QVERIFY(fixture.row());
	QVERIFY(fixture.row(u"c1"_s));
	QCOMPARE(fixture.row()->groupPts, qint64(6));
}

void TestGroups::catchUpSendsWhatIsHeld() {
	auto fixture = Fixture();
	fixture.harness->on(u"groups.list"_s, [](const Call &) {
		return Reply::Ok(QJsonObject{ { u"groups"_s, QJsonArray{
			Entry(Header(kGroup, 6, 4), MessageJson(kGroup, 4, u"u_sara"_s, u"old"_s)),
			Entry(Header(kOther, 3, 2), MessageJson(kOther, 2, u"u_ali"_s, u"x"_s)),
		} } });
	});
	QVERIFY(fixture.harness->start());
	QVERIFY(Test::WaitFor([&] { return fixture.row(kOther) != nullptr; }));
	fixture.harness->on(u"groups.sync"_s, [](const Call &) {
		return Reply::Ok(QJsonObject{
			{ u"groups"_s, QJsonArray{
				Entry(
					Header(kGroup, 9, 7, { { u"unread"_s, 3 }, { u"last_read_seq"_s, 4 }, { u"title"_s, u"تازه"_s } }),
					MessageJson(kGroup, 7, u"u_ali"_s, u"new"_s)),
			} },
			{ u"removed"_s, QJsonArray{ kOther, u"j1never"_s } },
			{ u"count"_s, 1 },
		});
	});
	auto removed = QSignalSpy(&fixture.groups(), &Groups::groupRemoved);
	fixture.harness->transport.setOnline(true);
	fixture.frame(Hello(100));
	QVERIFY(Test::WaitFor([&] { return fixture.groups().pts(kGroup) == 9; }));

	// Every group held went out with the pts applied to it.
	const auto bodies = fixture.harness->bodies(u"groups.sync"_s);
	QCOMPARE(bodies.size(), 1);
	auto known = QMap<QString, qint64>();
	for (const auto &item : bodies[0].value("known"_L1).toArray()) {
		known.insert(item.toObject().value("group"_L1).toString(), Json::Int64(item.toObject(), "pts"_L1));
	}
	QCOMPARE(known, (QMap<QString, qint64>{ { kGroup, 6 }, { kOther, 3 } }));

	const auto row = fixture.row();
	QCOMPARE(row->title, u"تازه"_s);
	QCOMPARE(row->unread, 3);
	QCOMPARE(row->lastSeq, qint64(7));
	QVERIFY(row->lastMessage && row->lastMessage->text == u"new");
	// Listed by me, but I am not a member any more: gone, and said so once.
	QVERIFY(!fixture.row(kOther));
	QCOMPARE(removed.size(), 1);
	QCOMPARE(removed[0][0].toString(), kOther);
	QCOMPARE(removed[0][1].toString(), u"removed"_s);
	// One call covered everything: no history was read.
	QCOMPARE(fixture.count(u"groups.history"_s), 0);
	QCOMPARE(fixture.count(u"groups.difference"_s), 0);
}

void TestGroups::nextFrameIsApplied() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	auto received = QSignalSpy(&fixture.groups(), &Groups::messageReceived);
	fixture.frame(GroupFrame(kGroup, 7, u"message"_s, MessageJson(kGroup, 5, u"u_ali"_s, u"تازه"_s)));
	const auto row = fixture.row();
	QCOMPARE(row->groupPts, qint64(7));
	QCOMPARE(row->lastSeq, qint64(5));
	QCOMPARE(row->unread, 1);
	QVERIFY(row->lastMessage && row->lastMessage->text == u"تازه");
	QCOMPARE(received.size(), 1);
	// Nothing was asked: the frame carried everything.
	QCOMPARE(fixture.count(u"groups.sync"_s), 0);
	QCOMPARE(fixture.count(u"groups.difference"_s), 0);

	// My own message from another device reads the group.
	fixture.frame(GroupFrame(kGroup, 8, u"message"_s, MessageJson(kGroup, 6, u"u_me"_s, u"من"_s)));
	QCOMPARE(fixture.row()->unread, 0);
	QCOMPARE(fixture.row()->lastReadSeq, qint64(6));
	QVERIFY(fixture.row()->lastMine);
	QCOMPARE(received.size(), 1);

	// The group on screen does not count unread.
	fixture.harness->chats().setVisibleChat(kGroup);
	fixture.frame(GroupFrame(kGroup, 9, u"message"_s, MessageJson(kGroup, 7, u"u_ali"_s, u"روی صفحه"_s)));
	QCOMPARE(fixture.row()->unread, 0);
	QCOMPARE(fixture.row()->groupPts, qint64(9));
}

void TestGroups::oldFrameIsDropped() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	fixture.frame(GroupFrame(kGroup, 6, u"message"_s, MessageJson(kGroup, 4, u"u_sara"_s, u"again"_s)));
	fixture.frame(GroupFrame(kGroup, 2, u"message"_s, MessageJson(kGroup, 2, u"u_sara"_s, u"older"_s)));
	const auto row = fixture.row();
	QCOMPARE(row->groupPts, qint64(6));
	QCOMPARE(row->unread, 0);
	QVERIFY(row->lastMessage && row->lastMessage->text == u"سلام به همه");
	QCOMPARE(fixture.count(u"groups.sync"_s), 0);
}

void TestGroups::gapWithoutCacheAsksTheCatchUp() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	fixture.harness->on(u"groups.sync"_s, [](const Call &) {
		return Reply::Ok(QJsonObject{
			{ u"groups"_s, QJsonArray{
				Entry(
					Header(kGroup, 9, 7, { { u"unread"_s, 3 }, { u"last_read_seq"_s, 4 } }),
					MessageJson(kGroup, 7, u"u_ali"_s, u"هفت"_s)),
			} },
			{ u"count"_s, 1 },
		});
	});
	// 9 arrives while 7 and 8 never did: nothing is cached, so no difference
	// could repair anything — the catch-up returns the row.
	fixture.frame(GroupFrame(kGroup, 9, u"message"_s, MessageJson(kGroup, 7, u"u_ali"_s, u"هفت"_s)));
	QVERIFY(Test::WaitFor([&] { return fixture.groups().pts(kGroup) == 9; }));
	QCOMPARE(fixture.count(u"groups.sync"_s), 1);
	QCOMPARE(fixture.count(u"groups.difference"_s), 0);
	QCOMPARE(fixture.row()->unread, 3);
	// A frame without data only says «the group moved».
	fixture.frame(GroupFrame(kGroup, 10, u"message"_s));
	QVERIFY(Test::WaitFor([&] { return fixture.count(u"groups.sync"_s) == 2; }));
}

void TestGroups::unknownGroupAsksTheCatchUpOnce() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	// A membership this device has not heard of yet.
	for (auto i = 0; i != 5; ++i) {
		fixture.frame(GroupFrame(kOther, 3 + i, u"message"_s, MessageJson(kOther, 2 + i, u"u_ali"_s, u"x"_s)));
	}
	Test::Settle(50);
	QCOMPARE(fixture.count(u"groups.sync"_s), 1);
}

void TestGroups::openAttachesTheCache() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup(2));
	QCOMPARE(fixture.row()->unread, 2);
	QVERIFY(fixture.open());
	const auto bodies = fixture.harness->bodies(u"groups.history"_s);
	QCOMPARE(bodies.size(), 1);
	QCOMPARE(bodies[0].value("group"_L1).toString(), kGroup);
	QVERIFY(!bodies[0].contains("chat"_L1));
	QCOMPARE(fixture.timeline(), (QStringList{ u"1=m1"_s, u"2=m2"_s, u"3=m3"_s, u"4=m4"_s }));
	// The page is the cache, current at the header's pts.
	QCOMPARE(fixture.row()->groupCachePts, qint64(6));
	QCOMPARE(fixture.row()->groupPts, qint64(6));
	QVERIFY(fixture.row()->isGroup);
	QVERIFY(!fixture.harness->messages().history(kGroup).items.front().mine);
	Test::Settle(30);
	QCOMPARE(fixture.count(u"groups.difference"_s), 0);

	// The next change extends the timeline as it comes.
	fixture.frame(GroupFrame(kGroup, 7, u"message"_s, MessageJson(kGroup, 5, u"u_ali"_s, u"m5"_s)));
	QCOMPARE(fixture.timeline().back(), u"5=m5"_s);
	QCOMPARE(fixture.row()->groupCachePts, qint64(7));

	// Marking read goes to the group's route.
	fixture.harness->messages().markRead(kGroup, 5);
	const auto read = fixture.harness->bodies(u"groups.read"_s);
	QCOMPARE(read.size(), 1);
	QCOMPARE(read[0].value("group"_L1).toString(), kGroup);
	QCOMPARE(Json::Int64(read[0], "seq"_L1), qint64(5));
	QCOMPARE(fixture.row()->unread, 0);
	QCOMPARE(fixture.count(u"chat.read"_s), 0);
}

void TestGroups::gapWithCacheReadsTheDifference() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	QVERIFY(fixture.open());
	fixture.harness->on(u"groups.difference"_s, [](const Call &call) {
		// Ordered by time of change, not by seq; each message once, as it is now.
		Q_ASSERT(call.body.value("pts"_L1).toString() == u"6");
		return Reply::Ok(QJsonObject{
			{ u"pts"_s, 9 },
			{ u"messages"_s, QJsonArray{
				MessageJson(kGroup, 5, u"u_ali"_s, u"m5"_s),
				MessageJson(kGroup, 3, u"u_sara"_s, u"m3 edited"_s, { { u"edited"_s, true }, { u"my_reaction"_s, u"👍"_s }, { u"reactions"_s, QJsonObject{ { u"👍"_s, 1 } } } }),
				MessageJson(kGroup, 2, u"u_ali"_s, QString(), { { u"deleted"_s, true } }),
			} },
			{ u"more"_s, false },
			{ u"too_long"_s, false },
		});
	});
	// 9 arrives, 7 and 8 were lost.
	fixture.frame(GroupFrame(kGroup, 9, u"message"_s, MessageJson(kGroup, 5, u"u_ali"_s, u"m5"_s)));
	QVERIFY(Test::WaitFor([&] { return fixture.groups().pts(kGroup) == 9; }));
	QCOMPARE(fixture.count(u"groups.difference"_s), 1);
	QCOMPARE(fixture.count(u"groups.sync"_s), 0);
	QCOMPARE(fixture.timeline(), (QStringList{ u"1=m1"_s, u"3=m3 edited"_s, u"4=m4"_s, u"5=m5"_s }));
	QCOMPARE(fixture.harness->messages().find(kGroup, 3)->myReaction, u"👍"_s);
	const auto row = fixture.row();
	QCOMPARE(row->groupCachePts, qint64(9));
	QCOMPARE(row->lastSeq, qint64(5));
	QCOMPARE(row->unread, 1); // only the new message of somebody else
	QVERIFY(row->lastMessage && row->lastMessage->text == u"m5");
	// Applying the same answer again changes nothing: the late frames are old.
	fixture.frame(GroupFrame(kGroup, 7, u"edited"_s, MessageJson(kGroup, 3, u"u_sara"_s, u"m3 edited"_s)));
	fixture.frame(GroupFrame(kGroup, 8, u"deleted"_s, MessageJson(kGroup, 2, u"u_ali"_s, QString(), { { u"deleted"_s, true } })));
	QCOMPARE(fixture.count(u"groups.difference"_s), 1);
	QCOMPARE(fixture.timeline().size(), 4);
}

void TestGroups::framesWaitForTheDifference() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	QVERIFY(fixture.open());
	fixture.harness->on(u"groups.difference"_s, [](const Call &) {
		return Reply::Hold();
	});
	fixture.frame(GroupFrame(kGroup, 8, u"message"_s, MessageJson(kGroup, 6, u"u_ali"_s, u"m6"_s)));
	QVERIFY(Test::WaitFor([&] { return fixture.count(u"groups.difference"_s) == 1; }));
	// While it is on its way the frames of this group queue up.
	fixture.frame(GroupFrame(kGroup, 9, u"message"_s, MessageJson(kGroup, 7, u"u_sara"_s, u"m7"_s)));
	fixture.frame(GroupFrame(kGroup, 10, u"message"_s, MessageJson(kGroup, 8, u"u_sara"_s, u"m8"_s)));
	QCOMPARE(fixture.row()->groupPts, qint64(6));
	QCOMPARE(fixture.timeline().size(), 4);
	// The answer knows up to 9; frame 9 is old by then, frame 10 is next.
	fixture.harness->transport.resolve(
		fixture.harness->transport.lastIndexOf(u"groups.difference"_s),
		QJsonObject{
			{ u"pts"_s, 9 },
			{ u"messages"_s, QJsonArray{
				MessageJson(kGroup, 7, u"u_sara"_s, u"m7"_s),
				MessageJson(kGroup, 6, u"u_ali"_s, u"m6"_s),
				MessageJson(kGroup, 5, u"u_ali"_s, u"m5"_s),
			} },
		});
	QVERIFY(Test::WaitFor([&] { return fixture.groups().pts(kGroup) == 10; }));
	QCOMPARE(fixture.count(u"groups.difference"_s), 1);
	QCOMPARE(fixture.timeline(), (QStringList{
		u"1=m1"_s, u"2=m2"_s, u"3=m3"_s, u"4=m4"_s, u"5=m5"_s, u"6=m6"_s, u"7=m7"_s, u"8=m8"_s,
	}));
	QCOMPARE(fixture.row()->groupCachePts, qint64(10));
	QCOMPARE(fixture.row()->unread, 4);
}

void TestGroups::tooLongStartsOver() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	QVERIFY(fixture.open());
	fixture.harness->on(u"groups.difference"_s, [](const Call &) {
		return Reply::Ok(QJsonObject{
			{ u"pts"_s, 900 },
			{ u"group"_s, Header(kGroup, 900, 600) },
			{ u"too_long"_s, true },
		});
	});
	fixture.harness->on(u"groups.history"_s, [](const Call &) {
		return Reply::Ok(QJsonObject{
			{ u"group"_s, Header(kGroup, 900, 600) },
			{ u"messages"_s, QJsonArray{
				MessageJson(kGroup, 599, u"u_ali"_s, u"m599"_s),
				MessageJson(kGroup, 600, u"u_sara"_s, u"m600"_s),
			} },
			{ u"has_more_before"_s, true },
		});
	});
	fixture.frame(GroupFrame(kGroup, 900, u"message"_s, MessageJson(kGroup, 600, u"u_sara"_s, u"m600"_s)));
	QVERIFY(Test::WaitFor([&] { return fixture.row()->groupCachePts == 900; }));
	// The cached tail went; the newest page was read again.
	QCOMPARE(fixture.count(u"groups.history"_s), 2);
	QCOMPARE(fixture.timeline(), (QStringList{ u"599=m599"_s, u"600=m600"_s }));
	QCOMPARE(fixture.row()->groupPts, qint64(900));
	QCOMPARE(fixture.row()->lastSeq, qint64(600));
}

void TestGroups::editsDeletesAndReactions() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	QVERIFY(fixture.open());
	// My reaction is set when I tap; frames never carry it.
	fixture.harness->messages().react(kGroup, 3, u"❤️"_s);
	QCOMPARE(fixture.harness->bodies(u"groups.react"_s).size(), 1);
	QCOMPARE(fixture.harness->bodies(u"groups.react"_s)[0].value("group"_L1).toString(), kGroup);
	fixture.frame(GroupFrame(kGroup, 7, u"reaction"_s, QJsonObject{
		{ u"group"_s, kGroup },
		{ u"seq"_s, 3 },
		{ u"reactions"_s, QJsonObject{ { u"❤️"_s, 2 } } },
	}));
	auto message = fixture.harness->messages().find(kGroup, 3);
	QCOMPARE(message->reactions.value(u"❤️"_s), qint64(2));
	QCOMPARE(message->myReaction, u"❤️"_s);
	// An edit keeps my reaction.
	fixture.frame(GroupFrame(kGroup, 8, u"edited"_s, MessageJson(kGroup, 3, u"u_sara"_s, u"m3!"_s, {
		{ u"edited"_s, true },
		{ u"reactions"_s, QJsonObject{ { u"❤️"_s, 2 } } },
	})));
	message = fixture.harness->messages().find(kGroup, 3);
	QCOMPARE(message->text, u"m3!"_s);
	QCOMPARE(message->myReaction, u"❤️"_s);
	// A delete for everyone takes the message out; the last one moves the preview.
	fixture.frame(GroupFrame(kGroup, 9, u"deleted"_s, MessageJson(kGroup, 4, u"u_ali"_s, QString(), { { u"deleted"_s, true } })));
	QVERIFY(!fixture.harness->messages().find(kGroup, 4));
	QVERIFY(fixture.row()->lastMessage && fixture.row()->lastMessage->seq == 3);
	// Several at once.
	fixture.frame(GroupFrame(kGroup, 10, u"deleted_many"_s, QJsonObject{
		{ u"group"_s, kGroup },
		{ u"seqs"_s, QJsonArray{ 1, 2 } },
	}));
	QCOMPARE(fixture.timeline(), (QStringList{ u"3=m3!"_s }));
	QCOMPARE(fixture.row()->groupPts, qint64(10));
	QCOMPARE(fixture.row()->groupCachePts, qint64(10));
	QCOMPARE(fixture.count(u"groups.difference"_s), 0);

	// Editing and deleting go to the group's routes.
	fixture.harness->messages().deleteMessage(kGroup, 3, true);
	const auto deleted = fixture.harness->bodies(u"groups.delete"_s);
	QCOMPARE(deleted.size(), 1);
	QCOMPARE(deleted[0].value("scope"_L1).toString(), u"everyone"_s);
	QCOMPARE(fixture.count(u"chat.delete"_s), 0);
}

void TestGroups::notesMoveTheHeader() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	auto members = QSignalSpy(&fixture.groups(), &Groups::membersChanged);
	fixture.frame(GroupFrame(kGroup, 7, u"message"_s, Note(kGroup, 5, u"u_sara"_s, u"member_added:u_reza"_s)));
	QCOMPARE(fixture.row()->group->memberCount, 4);
	fixture.frame(GroupFrame(kGroup, 8, u"message"_s, Note(kGroup, 6, u"u_ali"_s, u"member_left"_s)));
	QCOMPARE(fixture.row()->group->memberCount, 3);
	QCOMPARE(members.size(), 2);
	// A pin: the newest comes first; an unpin takes it away.
	fixture.frame(GroupFrame(kGroup, 9, u"message"_s, Note(kGroup, 7, u"u_sara"_s, u"pinned:2"_s)));
	fixture.frame(GroupFrame(kGroup, 10, u"message"_s, Note(kGroup, 8, u"u_sara"_s, u"pinned:4"_s)));
	QCOMPARE(fixture.row()->group->pins, (QVector<qint64>{ 4, 2 }));
	fixture.frame(GroupFrame(kGroup, 11, u"unpinned"_s, QJsonObject{ { u"group"_s, kGroup }, { u"seq"_s, 2 } }));
	QCOMPARE(fixture.row()->group->pins, (QVector<qint64>{ 4 }));
	// A new title is announced by a note: the header says which.
	fixture.harness->on(u"groups.get"_s, [](const Call &) {
		return Reply::Ok(Header(kGroup, 12, 9, { { u"title"_s, u"نام تازه"_s }, { u"member_count"_s, 3 }, { u"pins"_s, QJsonArray{ 4 } } }));
	});
	fixture.frame(GroupFrame(kGroup, 12, u"message"_s, Note(kGroup, 9, u"u_sara"_s, u"title_changed"_s)));
	QVERIFY(Test::WaitFor([&] { return fixture.row()->title == u"نام تازه"; }));
	QCOMPARE(fixture.count(u"groups.get"_s), 1);
	// The description and the member count of a large group arrive as `info`.
	fixture.frame(GroupFrame(kGroup, 13, u"info"_s, QJsonObject{
		{ u"group"_s, kGroup },
		{ u"member_count"_s, 240 },
		{ u"about"_s, u"توضیح تازه"_s },
	}));
	QCOMPARE(fixture.row()->group->memberCount, 240);
	QCOMPARE(fixture.row()->group->about, u"توضیح تازه"_s);
	// A note this build has no words for still counts as a change.
	fixture.frame(GroupFrame(kGroup, 14, u"message"_s, Note(kGroup, 10, u"u_sara"_s, u"something_newer:x"_s)));
	QCOMPARE(fixture.row()->groupPts, qint64(14));
	QCOMPARE(fixture.row()->lastSeq, qint64(10));
	// …and so does a kind of frame it does not know.
	fixture.frame(GroupFrame(kGroup, 15, u"later_kind"_s, QJsonObject{ { u"group"_s, kGroup } }));
	QCOMPARE(fixture.row()->groupPts, qint64(15));
	QCOMPARE(fixture.count(u"groups.sync"_s), 0);
}

void TestGroups::mentionsAreCounted() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	QVERIFY(fixture.open());
	auto received = QSignalSpy(&fixture.groups(), &Groups::messageReceived);
	// Muted: an ordinary message does not alert, one that addresses me does.
	fixture.harness->chats().setMuted(kGroup, true);
	QCOMPARE(fixture.harness->bodies(u"groups.member"_s).size(), 1);
	fixture.frame(GroupFrame(kGroup, 7, u"message"_s, MessageJson(kGroup, 5, u"u_ali"_s, u"سلام"_s)));
	QCOMPARE(received.size(), 0);
	QCOMPARE(fixture.row()->unreadMentions, 0);
	fixture.frame(GroupFrame(kGroup, 8, u"message"_s, MessageJson(kGroup, 6, u"u_ali"_s, u"سلام هادی"_s, {
		{ u"entities"_s, QJsonArray{ QJsonObject{
			{ u"type"_s, u"mention"_s },
			{ u"offset"_s, 5 },
			{ u"length"_s, 4 },
			{ u"user"_s, u"u_me"_s },
		} } },
	})));
	QCOMPARE(fixture.row()->unreadMentions, 1);
	QCOMPARE(received.size(), 1);
	QCOMPARE(received[0][1].toBool(), true);
	QCOMPARE(fixture.harness->messages().find(kGroup, 6)->entities[0].user, u"u_me"_s);
	// A reply to a message I wrote addresses me too.
	fixture.frame(GroupFrame(kGroup, 9, u"message"_s, MessageJson(kGroup, 7, u"u_me"_s, u"من"_s)));
	fixture.frame(GroupFrame(kGroup, 10, u"message"_s, MessageJson(kGroup, 8, u"u_sara"_s, u"جواب"_s, { { u"reply_to_seq"_s, 7 } })));
	QCOMPARE(fixture.row()->unreadMentions, 1); // my own send read the group, then one more
	QCOMPARE(received.size(), 2);
	// Reading to the end clears them.
	fixture.harness->messages().markRead(kGroup, 8);
	QCOMPARE(fixture.row()->unreadMentions, 0);
}

void TestGroups::sendGoesToTheGroupRoute() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	QVERIFY(fixture.open());
	fixture.harness->on(u"groups.send"_s, [](const Call &) {
		return Reply::Ok(QJsonObject{
			{ u"message_id"_s, u"m_new"_s },
			{ u"seq"_s, 5 },
			{ u"pts"_s, 7 },
			{ u"created_at"_s, 1791724999000. },
		});
	});
	auto options = Outbox::TextOptions();
	options.silent = true;
	auto mention = Entity();
	mention.type = u"mention"_s;
	mention.offset = 5;
	mention.length = 4;
	mention.user = u"u_sara"_s;
	options.entities = { mention };
	const auto id = fixture.harness->outbox().sendText(kGroup, u"سلام سارا"_s, options);
	QVERIFY(Test::WaitFor([&] { return fixture.groups().pts(kGroup) == 7; }));
	const auto bodies = fixture.harness->bodies(u"groups.send"_s);
	QCOMPARE(bodies.size(), 1);
	QCOMPARE(bodies[0].value("group"_L1).toString(), kGroup);
	QVERIFY(!bodies[0].contains("chat"_L1)); // a strict body: nothing the route does not know
	QCOMPARE(bodies[0].value("client_message_id"_L1).toString(), id);
	QCOMPARE(bodies[0].value("silent"_L1).toBool(), true);
	const auto entity = bodies[0].value("entities"_L1).toArray()[0].toObject();
	QCOMPARE(entity.value("type"_L1).toString(), u"mention"_s);
	QCOMPARE(entity.value("user"_L1).toString(), u"u_sara"_s);
	QCOMPARE(entity.value("offset"_L1).toInt(), 5);
	QCOMPARE(fixture.count(u"chat.send"_s), 0);
	// The message is the next change: both counters move, sending reads the group.
	QCOMPARE(fixture.row()->groupCachePts, qint64(7));
	QCOMPARE(fixture.row()->lastSeq, qint64(5));
	QCOMPARE(fixture.row()->lastReadSeq, qint64(5));
	QCOMPARE(fixture.timeline().back(), u"5=سلام سارا"_s);
	QCOMPARE(fixture.count(u"groups.difference"_s), 0);
}

void TestGroups::sendAnswerThenFrame() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	QVERIFY(fixture.open());
	fixture.harness->on(u"groups.send"_s, [](const Call &) {
		return Reply::Ok(QJsonObject{ { u"message_id"_s, u"m_new"_s }, { u"seq"_s, 5 }, { u"pts"_s, 7 } });
	});
	const auto id = fixture.harness->outbox().sendText(kGroup, u"mine"_s);
	QVERIFY(Test::WaitFor([&] { return fixture.groups().pts(kGroup) == 7; }));
	// The live frame of my own message arrives too: its pts is stored already.
	fixture.frame(GroupFrame(kGroup, 7, u"message"_s, MessageJson(kGroup, 5, u"u_me"_s, u"mine"_s, { { u"client_message_id"_s, id } })));
	QCOMPARE(fixture.timeline(), (QStringList{ u"1=m1"_s, u"2=m2"_s, u"3=m3"_s, u"4=m4"_s, u"5=mine"_s }));
	QCOMPARE(fixture.count(u"groups.difference"_s), 0);
}

void TestGroups::frameThenSendAnswer() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	QVERIFY(fixture.open());
	fixture.harness->on(u"groups.send"_s, [](const Call &) {
		return Reply::Hold();
	});
	const auto id = fixture.harness->outbox().sendText(kGroup, u"mine"_s);
	QVERIFY(Test::WaitFor([&] { return fixture.count(u"groups.send"_s) == 1; }));
	QCOMPARE(fixture.timeline().size(), 5); // the pending bubble
	// The frame comes first and is the message: the bubble stops «sending».
	fixture.frame(GroupFrame(kGroup, 7, u"message"_s, MessageJson(kGroup, 5, u"u_me"_s, u"mine"_s, { { u"client_message_id"_s, id } })));
	QCOMPARE(fixture.timeline(), (QStringList{ u"1=m1"_s, u"2=m2"_s, u"3=m3"_s, u"4=m4"_s, u"5=mine"_s }));
	QCOMPARE(fixture.row()->groupPts, qint64(7));
	// The answer finds its pts stored.
	fixture.harness->transport.resolve(
		fixture.harness->transport.lastIndexOf(u"groups.send"_s),
		QJsonObject{ { u"message_id"_s, u"m_new"_s }, { u"seq"_s, 5 }, { u"pts"_s, 7 } });
	Test::Settle(50);
	QCOMPARE(fixture.timeline().size(), 5);
	QCOMPARE(fixture.count(u"groups.difference"_s), 0);
	QCOMPARE(fixture.harness->outbox().pendingCount(kGroup), 0);
}

void TestGroups::slowModeKeepsTheMessageQueued() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	QVERIFY(fixture.open());
	auto attempt = 0;
	fixture.harness->on(u"groups.send"_s, [&](const Call &) {
		if (!attempt++) {
			auto reply = Reply::Fail(429, u"group.slow_mode"_s, true);
			reply.error.retryAfter = 1.;
			return reply;
		}
		return Reply::Ok(QJsonObject{ { u"message_id"_s, u"m_new"_s }, { u"seq"_s, 5 }, { u"pts"_s, 7 } });
	});
	fixture.harness->on(u"groups.get"_s, [](const Call &) {
		return Reply::Ok(Header(kGroup, 6, 4, { { u"can_send"_s, false }, { u"send_blocked"_s, u"slow_mode"_s }, { u"slow_mode_wait_s"_s, 1 } }));
	});
	auto failed = QSignalSpy(&fixture.harness->outbox(), &Outbox::failed);
	const auto id = fixture.harness->outbox().sendText(kGroup, u"mine"_s);
	QVERIFY(Test::WaitFor([&] { return fixture.count(u"groups.send"_s) == 1; }));
	Test::Settle(50);
	// Queued, not failed; the wait is known for the countdown.
	QCOMPARE(failed.size(), 0);
	QCOMPARE(fixture.harness->outbox().pendingCount(kGroup), 1);
	QVERIFY(fixture.groups().slowModeUntil(kGroup) > 0);
	// It goes with the same id when the wait is over.
	QVERIFY(Test::WaitFor([&] { return fixture.groups().pts(kGroup) == 7; }, 4000));
	const auto bodies = fixture.harness->bodies(u"groups.send"_s);
	QCOMPARE(bodies.size(), 2);
	QCOMPARE(bodies[1].value("client_message_id"_L1).toString(), id);
	QCOMPARE(failed.size(), 0);
}

void TestGroups::ownPlaceMembership() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	fixture.harness->transport.setOnline(true);
	fixture.frame(Hello(100));
	QVERIFY(Test::WaitFor([&] { return fixture.count(u"groups.sync"_s) == 1; }));
	Test::Settle(30);

	// Added to a group I do not hold: the catch-up brings it with its row.
	fixture.harness->on(u"groups.sync"_s, [](const Call &) {
		return Reply::Ok(QJsonObject{
			{ u"groups"_s, QJsonArray{
				Entry(Header(kOther, 2, 2, { { u"unread"_s, 1 }, { u"last_read_seq"_s, 1 } }), Note(kOther, 2, u"u_sara"_s, u"member_added:u_me"_s)),
			} },
			{ u"count"_s, 2 },
		});
	});
	fixture.frame(UserUpdate(101, QJsonObject{
		{ u"group"_s, kOther },
		{ u"status"_s, u"member"_s },
		{ u"role"_s, u"member"_s },
		{ u"by"_s, u"u_sara"_s },
	}));
	QVERIFY(Test::WaitFor([&] { return fixture.row(kOther) != nullptr; }));
	QCOMPARE(fixture.row(kOther)->unread, 1);

	// My role changed in a group I hold: the header says how.
	fixture.harness->on(u"groups.get"_s, [](const Call &) {
		return Reply::Ok(Header(kGroup, 6, 4, {
			{ u"role"_s, u"admin"_s },
			{ u"rights"_s, QJsonArray{ u"ban_users"_s } },
			{ u"join_requests"_s, 2 },
		}));
	});
	fixture.frame(UserUpdate(102, QJsonObject{
		{ u"group"_s, kGroup },
		{ u"status"_s, u"member"_s },
		{ u"role"_s, u"admin"_s },
	}));
	QVERIFY(Test::WaitFor([&] { return fixture.row()->group->role == u"admin"; }));
	QVERIFY(fixture.row()->group->hasRight(u"ban_users"));
	QCOMPARE(fixture.row()->group->joinRequests, 2);
	QCOMPARE(fixture.count(u"groups.get"_s), 1);

	// Removed: the group leaves the list and its screens close.
	auto removed = QSignalSpy(&fixture.groups(), &Groups::groupRemoved);
	fixture.frame(UserUpdate(103, QJsonObject{
		{ u"group"_s, kGroup },
		{ u"status"_s, u"kicked"_s },
		{ u"role"_s, u"member"_s },
		{ u"by"_s, u"u_sara"_s },
	}));
	QVERIFY(!fixture.row());
	QCOMPARE(removed.size(), 1);
	QCOMPARE(removed[0][1].toString(), u"kicked"_s);
	// The user-level pts moved with every one of them.
	QCOMPARE(fixture.harness->sync().pts().value_or(0), qint64(103));
}

void TestGroups::ownPlaceReadSettingsReaction() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup(2));
	QCOMPARE(fixture.row()->unread, 2);
	QVERIFY(fixture.open());
	fixture.harness->transport.setOnline(true);
	fixture.frame(Hello(100));

	// Read on another device, to the end.
	fixture.frame(UserUpdate(101, QJsonObject{ { u"group"_s, kGroup }, { u"read_seq"_s, 4 } }));
	QCOMPARE(fixture.row()->unread, 0);
	QCOMPARE(fixture.row()->lastReadSeq, qint64(4));

	// Settings changed elsewhere.
	fixture.frame(UserUpdate(102, QJsonObject{ { u"group"_s, kGroup }, { u"settings"_s, QJsonObject{
		{ u"muted"_s, true },
		{ u"muted_until"_s, 1791758353 },
		{ u"notify"_s, u"mentions"_s },
		{ u"archived"_s, false },
		{ u"pinned"_s, true },
		{ u"cleared_before_seq"_s, 2 },
	} } }));
	auto row = fixture.row();
	QVERIFY(row->muted && row->pinned && !row->archived);
	QCOMPARE(row->group->notify, u"mentions"_s);
	// The clear mark grew: the messages at or below it are gone.
	QCOMPARE(fixture.timeline(), (QStringList{ u"3=m3"_s, u"4=m4"_s }));

	// A message hidden for me, and my reaction set elsewhere.
	fixture.frame(UserUpdate(103, QJsonObject{ { u"group"_s, kGroup }, { u"hidden_seq"_s, 3 } }));
	QCOMPARE(fixture.timeline(), (QStringList{ u"4=m4"_s }));
	fixture.frame(UserUpdate(104, QJsonObject{ { u"group"_s, kGroup }, { u"reaction"_s, QJsonObject{
		{ u"seq"_s, 4 },
		{ u"emoji"_s, u"👍"_s },
	} } }));
	QCOMPARE(fixture.harness->messages().find(kGroup, 4)->myReaction, u"👍"_s);

	// A draft saved on another device, then used up by a message.
	fixture.frame(UserUpdate(105, QJsonObject{ { u"group"_s, kGroup }, { u"draft"_s, QJsonObject{
		{ u"text"_s, u"در حال نوشتن"_s },
		{ u"updated_at"_s, 1791747176015. },
	} } }));
	QVERIFY(fixture.row()->draft && fixture.row()->draft->text == u"در حال نوشتن");
	fixture.frame(UserUpdate(106, QJsonObject{ { u"group"_s, kGroup }, { u"draft"_s, QJsonObject{
		{ u"text"_s, QString() },
		{ u"updated_at"_s, 1791747176090. },
	} } }));
	QVERIFY(!fixture.row()->draft);
}

void TestGroups::notFoundDropsTheGroup() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	QVERIFY(fixture.open());
	// A message is queued behind one that is on its way.
	fixture.harness->on(u"groups.send"_s, [](const Call &) {
		return Reply::Fail(404, u"group.not_found"_s);
	});
	auto removed = QSignalSpy(&fixture.groups(), &Groups::groupRemoved);
	fixture.harness->outbox().sendText(kGroup, u"one"_s);
	fixture.harness->outbox().sendText(kGroup, u"two"_s);
	QVERIFY(Test::WaitFor([&] { return fixture.row() == nullptr; }));
	// I am not a member any more: the group is gone at once, and its queued
	// sends with it — none is tried again.
	QCOMPARE(removed.size(), 1);
	QCOMPARE(removed[0][1].toString(), u"removed"_s);
	QCOMPARE(fixture.harness->outbox().pendingCount(), 0);
	QVERIFY(fixture.harness->outbox().items().isEmpty());
	Test::Settle(100);
	QCOMPARE(fixture.count(u"groups.send"_s), 1);
}

void TestGroups::purgedFrameDropsTheGroup() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	auto removed = QSignalSpy(&fixture.groups(), &Groups::groupRemoved);
	fixture.frame(GroupFrame(kGroup, 7, u"purged"_s, QJsonObject{ { u"group"_s, kGroup } }));
	QVERIFY(!fixture.row());
	QCOMPARE(removed.size(), 1);
	QCOMPARE(removed[0][1].toString(), u"deleted"_s);
	// The update that says the same finds nothing left to do.
	fixture.harness->transport.setOnline(true);
	fixture.frame(Hello(100));
	fixture.frame(UserUpdate(101, QJsonObject{ { u"group"_s, kGroup }, { u"status"_s, u"deleted"_s } }));
	QCOMPARE(removed.size(), 1);
}

void TestGroups::actionsUseTheGroupRoutes() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	auto &transport = fixture.harness->transport;
	auto &chats = fixture.harness->chats();

	chats.setPinned(kGroup, true);
	QCOMPARE(fixture.harness->bodies(u"groups.pin"_s).size(), 1);
	QCOMPARE(transport.count(u"chat.pin"_s), 0);
	chats.setArchived(kGroup, true);
	QCOMPARE(fixture.harness->bodies(u"groups.member"_s).back().value("archived"_L1).toBool(), true);
	chats.setArchived(kGroup, false);
	// An explicit false must reach the server.
	QVERIFY(fixture.harness->bodies(u"groups.member"_s).back().contains("archived"_L1));
	chats.muteFor(kGroup, u"8h"_s);
	QCOMPARE(fixture.harness->bodies(u"groups.member"_s).back().value("mute_for"_L1).toString(), u"8h"_s);
	QCOMPARE(transport.count(u"chat.member"_s), 0);
	// Typing in a group is a group_typing signal (no capability).
	transport.setOnline(true);
	chats.sendTyping(kGroup);
	Test::Settle(30);
	QCOMPARE(transport.count(u"chat.capability"_s), 0);
	QCOMPARE(transport.groupTyping.size(), 1);
	QCOMPARE(transport.groupTyping[0].first, kGroup);
	QCOMPARE(transport.groupTyping[0].second, u"typing"_s);
	QVERIFY(transport.typing.isEmpty());

	// The draft of a group is saved through its own route.
	auto draft = Draft();
	draft.text = u"پیش‌نویس"_s;
	chats.saveDraft(kGroup, draft);
	QCOMPARE(fixture.harness->bodies(u"groups.draft.save"_s).size(), 1);
	QCOMPARE(transport.count(u"chat.draft.save"_s), 0);

	// Members, invite links, leaving.
	transport.handler = [prev = transport.handler](const Call &call) {
		if (call.method == u"groups.members") {
			return Reply::Ok(QJsonObject{
				{ u"members"_s, QJsonArray{
					QJsonObject{ { u"user"_s, u"u_sara"_s }, { u"role"_s, u"owner"_s }, { u"rights"_s, QJsonArray() }, { u"status"_s, u"member"_s } },
					QJsonObject{ { u"user"_s, u"u_me"_s }, { u"role"_s, u"member"_s }, { u"rights"_s, QJsonArray() }, { u"status"_s, u"member"_s } },
				} },
				{ u"next_cursor"_s, u"more"_s },
			});
		} else if (call.method == u"groups.invites.create") {
			return Reply::Ok(QJsonObject{
				{ u"link"_s, u"j1group.6LSbibQJp9aVabUoyMbHMQ"_s },
				{ u"url"_s, u"https://eleza.me/g/j1group.6LSbibQJp9aVabUoyMbHMQ"_s },
				{ u"uses"_s, 0 },
				{ u"max_uses"_s, 10 },
			});
		} else if (call.method == u"groups.invite.join") {
			return Reply::Ok(QJsonObject{ { u"pending"_s, true } });
		}
		return prev(call);
	};
	auto page = GroupMembersPage();
	auto got = false;
	fixture.groups().loadMembers(kGroup, u"all"_s, QString(), base::Guard(), [&](const GroupMembersPage &result) {
		page = result;
		got = true;
	});
	QVERIFY(Test::WaitFor([&] { return got; }));
	QCOMPARE(page.members.size(), 2);
	QCOMPARE(page.members[0].role, u"owner"_s);
	QCOMPARE(page.nextCursor, u"more"_s);
	QVERIFY(!fixture.harness->bodies(u"groups.members"_s)[0].contains("filter"_L1));

	auto request = GroupInviteRequest();
	request.name = u"همه"_s;
	request.maxUses = 10;
	auto invite = GroupInvite();
	got = false;
	fixture.groups().createInvite(kGroup, request, base::Guard(), [&](const GroupInvite &result) {
		invite = result;
		got = true;
	});
	QVERIFY(Test::WaitFor([&] { return got; }));
	QCOMPARE(invite.maxUses, 10);
	QCOMPARE(GroupInviteLinks::Token(invite.url), invite.link);
	const auto created = fixture.harness->bodies(u"groups.invites.create"_s)[0];
	QCOMPARE(created.value("name"_L1).toString(), u"همه"_s);
	QVERIFY(!created.contains("expires_in"_L1));

	auto joined = GroupJoinResult();
	got = false;
	fixture.groups().joinByInvite(u"j1x.6LSbibQJp9aVabUoyMbHMQ"_s, base::Guard(), [&](const GroupJoinResult &result) {
		joined = result;
		got = true;
	});
	QVERIFY(Test::WaitFor([&] { return got; }));
	QVERIFY(joined.pending && !joined.group);

	auto removed = QSignalSpy(&fixture.groups(), &Groups::groupRemoved);
	got = false;
	fixture.groups().leave(kGroup, base::Guard(), [&] { got = true; });
	QVERIFY(Test::WaitFor([&] { return got; }));
	QVERIFY(!fixture.row());
	QCOMPARE(removed.size(), 1);
	QCOMPARE(removed[0][1].toString(), u"left"_s);
}

void TestGroups::permissionsGoOverHttp() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	fixture.harness->http.handler = [](const Test::HttpServer::Request &request) {
		if (request.path == "/v1/groups/j1group/permissions") {
			return Test::HttpServer::Response::Ok(Header(kGroup, 7, 4, {
				{ u"default_permissions"_s, QJsonArray() },
				{ u"permissions"_s, QJsonArray() },
				{ u"can_send"_s, false },
				{ u"send_blocked"_s, u"restricted"_s },
			}));
		}
		return Test::HttpServer::Response::Ok();
	};
	auto done = false;
	// «Ordinary members may do nothing»: an empty list, which only HTTP can say.
	fixture.groups().setPermissions(kGroup, QStringList(), std::nullopt, base::Guard(), [&](const Group &) {
		done = true;
	});
	QVERIFY(Test::WaitFor([&] { return done; }));
	const auto requests = fixture.harness->http.to("/v1/groups/j1group/permissions");
	QCOMPARE(requests.size(), 1);
	QCOMPARE(requests[0].method, QByteArray("PUT"));
	const auto body = requests[0].json();
	QVERIFY(body.contains("default_permissions"_L1));
	QVERIFY(body.value("default_permissions"_L1).toArray().isEmpty());
	QVERIFY(!body.contains("history_visible"_L1));
	QCOMPARE(fixture.count(u"groups.permissions"_s), 0);
	QVERIFY(fixture.row()->group->defaultPermissions.isEmpty());
}

void TestGroups::cachedGroupIsRepairedOnOpen() {
	auto dir = QTemporaryDir();
	const auto path = dir.filePath(u"cache.db"_s);
	{
		auto fixture = Fixture(true, true, path);
		QVERIFY(fixture.startWithGroup());
		QVERIFY(fixture.open());
		QCOMPARE(fixture.row()->groupCachePts, qint64(6));
	}
	// The next run: the row and its messages come from the database.
	auto fixture = Fixture(true, true, path);
	fixture.harness->on(u"groups.sync"_s, [](const Call &) {
		// Three changes happened while the app was closed.
		return Reply::Ok(QJsonObject{
			{ u"groups"_s, QJsonArray{
				Entry(
					Header(kGroup, 9, 6, { { u"unread"_s, 2 }, { u"last_read_seq"_s, 4 } }),
					MessageJson(kGroup, 6, u"u_ali"_s, u"m6"_s)),
			} },
			{ u"count"_s, 1 },
		});
	});
	QVERIFY(fixture.harness->start());
	QVERIFY(Test::WaitFor([&] { return fixture.groups().pts(kGroup) == 9; }));
	QCOMPARE(fixture.count(u"groups.list"_s), 0); // a group is held: the catch-up, not the list
	// The row is right after one call; the cached messages are behind it.
	QCOMPARE(fixture.row()->unread, 2);
	QCOMPARE(fixture.row()->groupCachePts, qint64(6));
	QCOMPARE(fixture.count(u"groups.difference"_s), 0);

	// Opening it: the cache is shown, then one difference from where it is.
	fixture.harness->on(u"groups.difference"_s, [](const Call &call) {
		Q_ASSERT(call.body.value("pts"_L1).toString() == u"6");
		return Reply::Ok(QJsonObject{
			{ u"pts"_s, 9 },
			{ u"messages"_s, QJsonArray{
				MessageJson(kGroup, 6, u"u_ali"_s, u"m6"_s),
				MessageJson(kGroup, 5, u"u_sara"_s, u"m5"_s),
				MessageJson(kGroup, 1, u"u_sara"_s, u"m1 edited"_s, { { u"edited"_s, true } }),
			} },
		});
	});
	auto cached = -1;
	fixture.harness->messages().loadCached(kGroup, base::Guard(), [&](int count) { cached = count; });
	QVERIFY(Test::WaitFor([&] { return cached >= 0; }));
	QCOMPARE(cached, 4);
	QVERIFY(fixture.open(9, 6));
	QVERIFY(Test::WaitFor([&] { return fixture.row()->groupCachePts == 9; }));
	QCOMPARE(fixture.count(u"groups.difference"_s), 1);
	QCOMPARE(fixture.timeline(), (QStringList{
		u"1=m1 edited"_s, u"2=m2"_s, u"3=m3"_s, u"4=m4"_s, u"5=m5"_s, u"6=m6"_s,
	}));
}

void TestGroups::typingAndAdminSignals() {
	auto fixture = Fixture();
	QVERIFY(fixture.startWithGroup());
	auto &chats = fixture.harness->chats();
	const auto typing = [&](const QString &user, const QString &kind) {
		auto frame = Rt::Frame();
		frame.type = Type::GroupTyping;
		frame.chat = kGroup;
		frame.user = user;
		frame.kind = kind;
		frame.ttl = 6;
		fixture.frame(frame);
	};
	auto activity = QSignalSpy(&chats, &Chats::activityChanged);
	typing(u"u_sara"_s, u"recording_voice"_s);
	QVERIFY(chats.activity(kGroup).has_value());
	QCOMPARE(chats.activity(kGroup)->kind, u"recording_voice"_s);
	QCOMPARE(chats.activity(kGroup)->user, u"u_sara"_s);
	QCOMPARE(activity.size(), 1);
	// Somebody else's cancel does not end Sara's indicator, hers does.
	typing(u"u_ali"_s, u"cancel"_s);
	QVERIFY(chats.activity(kGroup).has_value());
	typing(u"u_sara"_s, u"cancel"_s);
	QVERIFY(!chats.activity(kGroup).has_value());
	// Mine from another device is not shown.
	typing(u"u_me"_s, u"typing"_s);
	QVERIFY(!chats.activity(kGroup).has_value());

	// The number of requests to join, live.
	auto requests = QSignalSpy(&fixture.groups(), &Groups::joinRequestsChanged);
	auto admin = Rt::Frame();
	admin.type = Type::GroupAdmin;
	admin.chat = kGroup;
	admin.kind = u"join_requests"_s;
	admin.count = 3;
	admin.user = u"u_new"_s;
	fixture.frame(admin);
	QCOMPARE(fixture.groups().group(kGroup)->joinRequests, 3);
	QCOMPARE(requests.size(), 1);
	QCOMPARE(requests[0][1].toString(), u"u_new"_s);
	admin.count = 2;
	admin.user = QString();
	fixture.frame(admin);
	QCOMPARE(fixture.groups().group(kGroup)->joinRequests, 2);
	// Not a group of mine: nothing.
	admin.chat = u"j_other"_s;
	fixture.frame(admin);
	QCOMPARE(requests.size(), 2);
	// A kind this client does not know: the header is read again.
	const auto gets = fixture.count(u"groups.get"_s);
	admin.chat = kGroup;
	admin.kind = u"something_new"_s;
	fixture.frame(admin);
	Test::Settle(30);
	QCOMPARE(fixture.count(u"groups.get"_s), gets + 1);
}

QTEST_GUILESS_MAIN(TestGroups)

#include "test_groups.moc"
