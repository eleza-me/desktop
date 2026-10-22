// The report / block bar (Data::ActionBar, Data::Chats::dismissActionBar):
// when it shows, the plans of its choices and how they run, and a × that
// waits on this device until the server has it.
#include "data/data_action_bar.h"
#include "data/data_json.h"
#include "test_harness.h"

#include <QtCore/QTemporaryDir>

using namespace Data;
using Test::Harness;
using Reply = Test::FakeTransport::Reply;
using Call = Test::FakeTransport::Call;
using Step = ActionBar::Step;

namespace {

[[nodiscard]] Chat Direct() {
	auto result = Chat();
	result.id = u"c9"_s;
	result.section = u"message"_s;
	result.peerUserId = u"u_spam"_s;
	result.state = u"active"_s;
	result.info.chat = result.id;
	result.info.kind = u"direct"_s;
	result.info.state = u"active"_s;
	result.info.actionBar = u"stranger"_s;
	return result;
}

[[nodiscard]] Chat GroupRow() {
	auto result = Chat();
	result.id = u"j1group"_s;
	result.isGroup = true;
	auto group = Group();
	group.id = result.id;
	group.status = u"member"_s;
	group.actionBar = u"added_by_stranger"_s;
	group.addedBy = u"u_spam"_s;
	result.group = group;
	return result;
}

[[nodiscard]] Api::Error Error(int status, const QString &code) {
	return Api::Error::Local(status, code, code);
}

// Runs a plan against answers given per step (absent: it goes through).
[[nodiscard]] ActionBar::Outcome RunWith(
		const QVector<Step> &plan,
		const QHash<int, Api::Error> &failures,
		QVector<Step> *calls = nullptr) {
	auto result = std::optional<ActionBar::Outcome>();
	ActionBar::Run(plan, [&](Step step, Fn<void(std::optional<Api::Error>)> finished) {
		if (calls) {
			calls->push_back(step);
		}
		const auto i = failures.constFind(int(step));
		finished((i != failures.cend()) ? std::make_optional(*i) : std::nullopt);
	}, [&](const ActionBar::Outcome &outcome) {
		result = outcome;
	});
	Q_ASSERT(result.has_value());
	return *result;
}

} // namespace

class TestActionBar final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void whenItShows();
	void plans();
	void runs();
	void failures();
	void dismissalWaitsForTheServer();

};

void TestActionBar::whenItShows() {
	const auto me = u"u_me"_s;
	QCOMPARE(ActionBar::For(Direct(), me), ActionBar::Kind::Stranger);
	auto chat = Direct();
	chat.info.actionBar = QString();
	QCOMPARE(ActionBar::For(chat, me), ActionBar::Kind::None);
	// A value this build does not know: no bar.
	chat.info.actionBar = u"something_new"_s;
	QCOMPARE(ActionBar::For(chat, me), ActionBar::Kind::None);
	// A request has its own accept / decline; a blocked or ended chat, Saved
	// Messages, a bot, a deleted account and a conversation have none.
	chat = Direct();
	chat.request = u"incoming"_s;
	QCOMPARE(ActionBar::For(chat, me), ActionBar::Kind::None);
	chat = Direct();
	chat.info.state = u"blocked"_s;
	QCOMPARE(ActionBar::For(chat, me), ActionBar::Kind::None);
	chat = Direct();
	chat.saved = true;
	QCOMPARE(ActionBar::For(chat, me), ActionBar::Kind::None);
	chat = Direct();
	chat.isBot = true;
	QCOMPARE(ActionBar::For(chat, me), ActionBar::Kind::None);
	chat = Direct();
	chat.peerDeleted = true;
	QCOMPARE(ActionBar::For(chat, me), ActionBar::Kind::None);
	chat = Direct();
	chat.isConversation = true;
	QCOMPARE(ActionBar::For(chat, me), ActionBar::Kind::None);

	QCOMPARE(ActionBar::For(GroupRow(), me), ActionBar::Kind::AddedByStranger);
	auto group = GroupRow();
	group.group->addedBy = QString(); // nobody to name or block
	QCOMPARE(ActionBar::For(group, me), ActionBar::Kind::None);
	group = GroupRow();
	group.group->addedBy = me;
	QCOMPARE(ActionBar::For(group, me), ActionBar::Kind::None);
	group = GroupRow();
	group.group->status = u"left"_s;
	QCOMPARE(ActionBar::For(group, me), ActionBar::Kind::None);
	// A group's bar is never read from the direct chat's field.
	group = GroupRow();
	group.group->actionBar = QString();
	group.info.actionBar = u"stranger"_s;
	QCOMPARE(ActionBar::For(group, me), ActionBar::Kind::None);
}

void TestActionBar::plans() {
	QCOMPARE(ActionBar::ChatBlockPlan(false), (QVector<Step>{ Step::Block }));
	QCOMPARE(ActionBar::ChatBlockPlan(true), (QVector<Step>{ Step::ReportChat, Step::Block }));
	QCOMPARE(ActionBar::ChatReportPlan(), (QVector<Step>{ Step::ReportChat, Step::Block }));
	// A report needs the membership: always first; the leave always last.
	QCOMPARE(ActionBar::GroupLeavePlan(false, false), (QVector<Step>{ Step::Leave }));
	QCOMPARE(
		ActionBar::GroupLeavePlan(true, true),
		(QVector<Step>{ Step::ReportGroup, Step::Block, Step::Leave }));
	QCOMPARE(ActionBar::GroupReportPlan(false, false), (QVector<Step>{ Step::ReportGroup }));
	QCOMPARE(
		ActionBar::GroupReportPlan(true, true),
		(QVector<Step>{ Step::ReportGroup, Step::Block, Step::Leave }));
	// «گزارش هرزنامه» of a group starts with «ترک گروه» ticked, the block not.
	QVERIFY(ActionBar::GroupReportDefaults().leave);
	QVERIFY(!ActionBar::GroupReportDefaults().block);
}

void TestActionBar::runs() {
	auto calls = QVector<Step>();
	auto outcome = RunWith(ActionBar::GroupLeavePlan(true, true), {}, &calls);
	QCOMPARE(calls, (QVector<Step>{ Step::ReportGroup, Step::Block, Step::Leave }));
	QCOMPARE(outcome.done, calls);
	QVERIFY(outcome.failed.isEmpty());
	QVERIFY(!outcome.error);
	QVERIFY(!outcome.nothingDone());

	// A duplicate report: I had reported it already — done.
	outcome = RunWith(ActionBar::ChatReportPlan(), {
		{ int(Step::ReportChat), Error(409, u"report.duplicate"_s) },
	});
	QVERIFY(outcome.did(Step::ReportChat));
	QVERIFY(outcome.did(Step::Block));
	QVERIFY(outcome.failed.isEmpty());
}

void TestActionBar::failures() {
	// The report that comes with a leave is best effort: the leave happens,
	// and the outcome says the report failed.
	auto calls = QVector<Step>();
	auto outcome = RunWith(ActionBar::GroupLeavePlan(true, true), {
		{ int(Step::ReportGroup), Error(503, u"server.unavailable"_s) },
	}, &calls);
	QCOMPARE(calls.size(), 3);
	QVERIFY(outcome.failedAt(Step::ReportGroup));
	QVERIFY(outcome.did(Step::Block));
	QVERIFY(outcome.did(Step::Leave));
	QCOMPARE(outcome.error->code, u"server.unavailable"_s);
	// A failed block does not keep me in a group I chose to leave.
	outcome = RunWith(ActionBar::GroupLeavePlan(true, false), {
		{ int(Step::Block), Error(0, u"offline"_s) },
	});
	QVERIFY(outcome.failedAt(Step::Block));
	QVERIFY(outcome.did(Step::Leave));
	// Nothing went through: the bar comes back.
	outcome = RunWith(ActionBar::ChatBlockPlan(true), {
		{ int(Step::ReportChat), Error(0, u"offline"_s) },
		{ int(Step::Block), Error(0, u"offline"_s) },
	});
	QVERIFY(outcome.nothingDone());
	QCOMPARE(outcome.failed.size(), 2);

	// What a failed × means: no answer, the server's trouble, too many calls,
	// a session being renewed wait; a refusal that would not change does not.
	QVERIFY(ActionBar::DismissWaits(Api::Error::Offline()));
	QVERIFY(ActionBar::DismissWaits(Error(503, u"server.unavailable"_s)));
	QVERIFY(ActionBar::DismissWaits(Error(429, u"rate_limited"_s)));
	QVERIFY(ActionBar::DismissWaits(Error(401, u"auth.expired"_s)));
	QVERIFY(ActionBar::DismissWaits(Error(408, u"timeout"_s)));
	QVERIFY(!ActionBar::DismissWaits(Error(404, u"chat.not_found"_s)));
	QVERIFY(!ActionBar::DismissWaits(Error(403, u"forbidden"_s)));
}

void TestActionBar::dismissalWaitsForTheServer() {
	auto dir = QTemporaryDir();
	QVERIFY(dir.isValid());
	const auto path = dir.filePath(u"cache.sqlite"_s);
	{
		auto harness = Harness(path);
		QVERIFY(harness.start());
		harness.chats().patch(u"c1"_s, [](Chat &c) {
			c.info.actionBar = u"stranger"_s;
		});
		QCOMPARE(harness.chats().chat(u"c1"_s)->info.actionBar, u"stranger"_s);
		harness.on(u"chat.action_bar.dismiss"_s, [](const Call &) {
			return Reply::Offline();
		});
		harness.chats().dismissActionBar(u"c1"_s);
		// Gone here at once.
		QVERIFY(harness.chats().chat(u"c1"_s)->info.actionBar.isEmpty());
		QVERIFY(Test::WaitFor([&] { return harness.transport.count(u"chat.action_bar.dismiss"_s) == 1; }));
		Test::Settle(30);
		// A header that still carries the bar (the server has not heard of
		// the ×) leaves it out while the × waits.
		auto info = harness.chats().chat(u"c1"_s)->info;
		info.actionBar = u"stranger"_s;
		harness.chats().applyInfo(info);
		QVERIFY(harness.chats().chat(u"c1"_s)->info.actionBar.isEmpty());
		const auto body = harness.bodies(u"chat.action_bar.dismiss"_s).front();
		QCOMPARE(body.value("chat"_L1).toString(), u"c1"_s);
	}
	{
		// The next start sends it again; once the server has it, it is done.
		auto harness = Harness(path);
		auto answered = 0;
		harness.on(u"chat.action_bar.dismiss"_s, [&](const Call &) {
			++answered;
			return Reply::Ok();
		});
		QVERIFY(harness.start());
		QVERIFY(Test::WaitFor([&] { return answered == 1; }));
		Test::Settle(30);
		auto info = harness.chats().chat(u"c1"_s)->info;
		info.actionBar = u"stranger"_s;
		harness.chats().applyInfo(info);
		QCOMPARE(harness.chats().chat(u"c1"_s)->info.actionBar, u"stranger"_s);
		// A refusal that would not change is given up at once.
		harness.on(u"chat.action_bar.dismiss"_s, [&](const Call &) {
			++answered;
			return Reply::Fail(404, u"chat.not_found"_s);
		});
		harness.chats().dismissActionBar(u"c1"_s);
		QVERIFY(Test::WaitFor([&] { return answered == 2; }));
		Test::Settle(30);
		harness.chats().flushActionBarDismissals();
		Test::Settle(30);
		QCOMPARE(answered, 2);
	}
}

QTEST_GUILESS_MAIN(TestActionBar)
#include "test_action_bar.moc"
