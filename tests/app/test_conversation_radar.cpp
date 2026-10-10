// Conversation::RadarModel on the store fakes: how the radar reacts to the
// conversation's state updates (joined / opened / ended / expired /
// continued), that it re-reads on a timer only while the connection is down,
// and that the expiry needs no network at all.
#include "conversation/conversation_radar_model.h"
#include "data/data_conversations.h"
#include "test_harness.h"

#include <QtTest/QSignalSpy>

using namespace Conversation;
using Test::Harness;
using Reply = Test::FakeTransport::Reply;
using Call = Test::FakeTransport::Call;

namespace {

const auto kGet = u"conv.get"_s;

[[nodiscard]] QJsonObject View(
		const QString &id,
		const QString &state,
		bool listener,
		qint64 expiresIn = 3600) {
	auto result = QJsonObject{
		{ u"id"_s, id },
		{ u"topic"_s, u"topic"_s },
		{ u"category"_s, u"heart"_s },
		{ u"state"_s, state },
		{ u"expires_at"_s, double(QDateTime::currentSecsSinceEpoch() + expiresIn) },
		{ u"speaker"_s, QJsonObject{
			{ u"ref"_s, u"p_a"_s },
			{ u"me"_s, true },
			{ u"role"_s, u"speaker"_s },
			{ u"masked"_s, true },
		} },
		{ u"my_role"_s, u"speaker"_s },
	};
	if (listener) {
		result.insert(u"listener"_s, QJsonObject{
			{ u"ref"_s, u"p_b"_s },
			{ u"masked"_s, true },
			{ u"mask_name"_s, u"x"_s },
			{ u"role"_s, u"listener"_s },
		});
	}
	return result;
}

// A harness whose `conv.get` answers what the test says it is now.
struct Fixture {
	explicit Fixture(const QString &id = u"conv9"_s, bool withConversationChat = false)
	: id(id) {
		view = View(id, u"open"_s, false);
		harness.on(kGet, [this](const Call &) {
			return fail ? Reply::Fail(failStatus, u"x.failed"_s) : Reply::Ok(view);
		});
		if (withConversationChat) {
			// The inbox has chat c4 that carries conversation conv1.
			harness.on(u"chat.inbox"_s, [](const Call &call) {
				const auto section = call.body.value("section"_L1).toString();
				return Reply::Ok((section == u"message")
					? Test::Fixture("inbox_message.json")
					: (section == u"conversation")
					? Test::Fixture("inbox_conversation.json")
					: QJsonObject{ { u"chats"_s, QJsonArray() } });
			});
		}
	}

	[[nodiscard]] int reads() const {
		return harness.transport.count(kGet);
	}
	void state(const QString &chat, const QString &state, const QString &event) {
		harness.session->conversations().notifyState(chat, state, std::nullopt, event);
	}

	QString id;
	Harness harness;
	QJsonObject view;
	bool fail = false;
	int failStatus = 500;
};

} // namespace

class TestConversationRadar : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void phaseOfView() {
		const auto now = QDateTime::currentMSecsSinceEpoch();
		auto conversation = Data::Conversation::FromJson(View(u"a"_s, u"open"_s, false));
		QCOMPARE(PhaseOf(conversation, now), RadarPhase::Waiting);
		conversation = Data::Conversation::FromJson(View(u"a"_s, u"active"_s, true));
		QCOMPARE(PhaseOf(conversation, now), RadarPhase::Matched);
		// Nobody came in time.
		conversation = Data::Conversation::FromJson(View(u"a"_s, u"open"_s, false, -5));
		QCOMPARE(PhaseOf(conversation, now), RadarPhase::Closed);
		// A matched conversation is not closed by the radar's clock.
		conversation = Data::Conversation::FromJson(View(u"a"_s, u"active"_s, true, -5));
		QCOMPARE(PhaseOf(conversation, now), RadarPhase::Matched);
		for (const auto state : { "ended", "expired", "closed", "purged", "readonly", "continued" }) {
			conversation = Data::Conversation::FromJson(View(u"a"_s, QString::fromLatin1(state), true));
			QCOMPARE(PhaseOf(conversation, now), RadarPhase::Closed);
			QVERIFY(IsEndedState(QString::fromLatin1(state)));
		}
		QVERIFY(!IsEndedState(u"open"_s));
		QVERIFY(!IsEndedState(u"active"_s));
	}

	void readsOnceThenWaits() {
		auto fixture = Fixture();
		QVERIFY(fixture.harness.start());
		fixture.harness.transport.setOnline(true);
		auto model = RadarModel(fixture.harness.session.get(), fixture.id);
		QCOMPARE(model.phase(), RadarPhase::Loading);
		model.setOfflinePollInterval(30);
		model.start();
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Waiting; }));
		QCOMPARE(fixture.reads(), 1);
		QVERIFY(model.conversation().has_value());
		// Connected: nothing is read again without an update.
		Test::Settle(150);
		QCOMPARE(fixture.reads(), 1);
		QVERIFY(!model.pollingOffline());
	}

	void joinedEventMatches() {
		auto fixture = Fixture();
		QVERIFY(fixture.harness.start());
		fixture.harness.transport.setOnline(true);
		auto model = RadarModel(fixture.harness.session.get(), fixture.id);
		auto matched = QSignalSpy(&model, &RadarModel::matched);
		auto closed = QSignalSpy(&model, &RadarModel::closed);
		model.start();
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Waiting; }));

		// The chat row of a fresh topic is not in the list yet: an update of
		// an unknown chat may be ours, the view is read.
		fixture.view = View(fixture.id, u"active"_s, true);
		fixture.state(u"c_new"_s, u"active"_s, u"joined"_s);
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Matched; }));
		QCOMPARE(fixture.reads(), 2);
		QCOMPARE(matched.size(), 1);
		QCOMPARE(closed.size(), 0);
		QVERIFY(model.conversation()->listener.has_value());

		// The same news twice does not match twice.
		fixture.state(u"c_new"_s, u"active"_s, u"opened"_s);
		QVERIFY(Test::WaitFor([&] { return fixture.reads() == 3; }));
		Test::Settle();
		QCOMPARE(matched.size(), 1);
	}

	void updatesOfOtherChatsAreIgnored() {
		auto fixture = Fixture();
		QVERIFY(fixture.harness.start());
		fixture.harness.transport.setOnline(true);
		auto model = RadarModel(fixture.harness.session.get(), fixture.id);
		model.start();
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Waiting; }));
		// c1 is a known direct chat: not our conversation.
		fixture.state(u"c1"_s, u"ended"_s, u"ended"_s);
		Test::Settle(60);
		QCOMPARE(fixture.reads(), 1);
		QCOMPARE(model.phase(), RadarPhase::Waiting);
	}

	void endedEventClosesWithoutReading() {
		// conv1 lives in chat c4 (the conversations inbox).
		auto fixture = Fixture(u"conv1"_s, true);
		QVERIFY(fixture.harness.start());
		QVERIFY(Test::WaitFor([&] {
			return !fixture.harness.chats().chatOfConversation(u"conv1"_s).isEmpty();
		}));
		fixture.harness.transport.setOnline(true);
		auto model = RadarModel(fixture.harness.session.get(), fixture.id);
		auto closed = QSignalSpy(&model, &RadarModel::closed);
		model.start();
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Waiting; }));
		const auto reads = fixture.reads();

		// Another conversation's chat ends: not ours.
		fixture.state(u"c1"_s, u"ended"_s, u"ended"_s);
		QCOMPARE(model.phase(), RadarPhase::Waiting);

		fixture.state(u"c4"_s, u"ended"_s, u"ended"_s);
		QCOMPARE(model.phase(), RadarPhase::Closed);
		QCOMPARE(model.closedState(), u"ended"_s);
		QCOMPARE(closed.size(), 1);
		QCOMPARE(closed.front().front().toString(), u"ended"_s);
		Test::Settle(40);
		QCOMPARE(fixture.reads(), reads);

		// Closed is final.
		fixture.state(u"c4"_s, u"active"_s, u"joined"_s);
		Test::Settle(40);
		QCOMPARE(model.phase(), RadarPhase::Closed);
		QCOMPARE(fixture.reads(), reads);
		QCOMPARE(closed.size(), 1);
	}

	void expiredAndContinuedEvents() {
		for (const auto event : { "expired", "continued" }) {
			auto fixture = Fixture(u"conv1"_s, true);
			QVERIFY(fixture.harness.start());
			QVERIFY(Test::WaitFor([&] {
				return !fixture.harness.chats().chatOfConversation(u"conv1"_s).isEmpty();
			}));
			fixture.harness.transport.setOnline(true);
			auto model = RadarModel(fixture.harness.session.get(), fixture.id);
			auto closed = QSignalSpy(&model, &RadarModel::closed);
			model.start();
			QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Waiting; }));
			// The state may still say "active" / "open": the event decides.
			fixture.state(u"c4"_s, u"open"_s, QString::fromLatin1(event));
			QCOMPARE(model.phase(), RadarPhase::Closed);
			QCOMPARE(model.closedState(), QString::fromLatin1(event));
			QCOMPARE(closed.size(), 1);
		}
	}

	void joinedOnKnownChatReads() {
		auto fixture = Fixture(u"conv1"_s, true);
		QVERIFY(fixture.harness.start());
		QVERIFY(Test::WaitFor([&] {
			return !fixture.harness.chats().chatOfConversation(u"conv1"_s).isEmpty();
		}));
		fixture.harness.transport.setOnline(true);
		fixture.view = View(u"conv1"_s, u"open"_s, false);
		auto model = RadarModel(fixture.harness.session.get(), fixture.id);
		model.start();
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Waiting; }));
		const auto reads = fixture.reads();
		fixture.view = View(u"conv1"_s, u"active"_s, true);
		fixture.state(u"c4"_s, u"active"_s, u"joined"_s);
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Matched; }));
		QCOMPARE(fixture.reads(), reads + 1);
	}

	void pollsOnlyWhileOffline() {
		auto fixture = Fixture();
		QVERIFY(fixture.harness.start());
		// The connection is down from the start.
		auto model = RadarModel(fixture.harness.session.get(), fixture.id);
		model.setOfflinePollInterval(25);
		model.start();
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Waiting; }));
		QVERIFY(model.pollingOffline());
		QVERIFY(Test::WaitFor([&] { return fixture.reads() >= 3; }));

		// Back online: one read for what was missed, then silence.
		fixture.harness.transport.setOnline(true);
		QVERIFY(!model.pollingOffline());
		Test::Settle(30);
		const auto reads = fixture.reads();
		Test::Settle(150);
		QCOMPARE(fixture.reads(), reads);

		// Down again: the fallback returns and finds the listener.
		fixture.view = View(fixture.id, u"active"_s, true);
		fixture.harness.transport.setOnline(false);
		QVERIFY(model.pollingOffline());
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Matched; }));
	}

	void reconnectReadsOnce() {
		auto fixture = Fixture();
		QVERIFY(fixture.harness.start());
		fixture.harness.transport.setOnline(true);
		auto model = RadarModel(fixture.harness.session.get(), fixture.id);
		model.setOfflinePollInterval(10'000);
		model.start();
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Waiting; }));
		fixture.harness.transport.setOnline(false);
		Test::Settle();
		QCOMPARE(fixture.reads(), 1);
		fixture.view = View(fixture.id, u"ended"_s, false);
		fixture.harness.transport.setOnline(true);
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Closed; }));
		QCOMPARE(fixture.reads(), 2);
		QCOMPARE(model.closedState(), u"ended"_s);
		QVERIFY(!model.pollingOffline());
	}

	void expiresLocally() {
		auto fixture = Fixture();
		fixture.view = View(fixture.id, u"open"_s, false, 1);
		QVERIFY(fixture.harness.start());
		fixture.harness.transport.setOnline(true);
		auto model = RadarModel(fixture.harness.session.get(), fixture.id);
		auto closed = QSignalSpy(&model, &RadarModel::closed);
		model.start();
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Waiting; }));
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Closed; }, 4000));
		QCOMPARE(model.closedState(), u"expired"_s);
		QCOMPARE(closed.size(), 1);
		// No request was needed to learn that.
		QCOMPARE(fixture.reads(), 1);
	}

	void initialViewShowsAtOnce() {
		auto fixture = Fixture();
		QVERIFY(fixture.harness.start());
		fixture.harness.transport.setOnline(true);
		auto model = RadarModel(fixture.harness.session.get(), fixture.id);
		model.setInitial(Data::Conversation::FromJson(fixture.view));
		QCOMPARE(model.phase(), RadarPhase::Waiting);
		// A view of another conversation is not taken.
		auto other = RadarModel(fixture.harness.session.get(), u"other"_s);
		other.setInitial(Data::Conversation::FromJson(fixture.view));
		QCOMPARE(other.phase(), RadarPhase::Loading);
	}

	void failures() {
		auto fixture = Fixture();
		fixture.fail = true;
		QVERIFY(fixture.harness.start());
		fixture.harness.transport.setOnline(true);
		auto model = RadarModel(fixture.harness.session.get(), fixture.id);
		model.start();
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Failed; }));
		QVERIFY(model.error().has_value());
		// Retry works.
		fixture.fail = false;
		model.reload();
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Waiting; }));
		QVERIFY(!model.error().has_value());

		// A failed re-read keeps what is on screen.
		fixture.fail = true;
		model.reload();
		QVERIFY(Test::WaitFor([&] { return !model.loading(); }));
		QCOMPARE(model.phase(), RadarPhase::Waiting);

		// Gone on the server: closed.
		fixture.failStatus = 404;
		model.reload();
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Closed; }));
		QCOMPARE(model.closedState(), u"ended"_s);
	}

	void hiddenScreenDoesNotRead() {
		auto fixture = Fixture();
		QVERIFY(fixture.harness.start());
		auto model = RadarModel(fixture.harness.session.get(), fixture.id);
		model.setOfflinePollInterval(20);
		model.start();
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Waiting; }));
		model.setActive(false);
		QVERIFY(!model.pollingOffline());
		Test::Settle(40);
		const auto reads = fixture.reads();
		fixture.view = View(fixture.id, u"active"_s, true);
		fixture.state(u"c_new"_s, u"active"_s, u"joined"_s);
		Test::Settle(80);
		QCOMPARE(fixture.reads(), reads);
		QCOMPARE(model.phase(), RadarPhase::Waiting);
		// Shown again: what was missed is read once.
		model.setActive(true);
		QVERIFY(Test::WaitFor([&] { return model.phase() == RadarPhase::Matched; }));
	}

};

QTEST_GUILESS_MAIN(TestConversationRadar)
#include "test_conversation_radar.moc"
