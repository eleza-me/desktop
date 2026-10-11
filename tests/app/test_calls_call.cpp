// Voice calls without a server and without audio: the state machine of one
// call (calls/calls_call.h), the rules about which call there is
// (calls/calls_instance.h) and the wire shapes (data/data_calls.h), against a
// scripted Data::CallsApi and a Calls::Media that connects when told to.
#include "calls/calls_call.h"
#include "calls/calls_instance.h"
#include "data/data_calls.h"
#include "rt/rt_client.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtTest/QtTest>

using Calls::Call;
using Calls::EndReason;
using Calls::State;
using Data::CallSignal;

namespace {

[[nodiscard]] QJsonObject Json(const char *text) {
	return QJsonDocument::fromJson(text).object();
}

[[nodiscard]] Data::CallJoin Join(
		const QString &call,
		const QString &state = u"ringing"_s,
		const QString &token = u"token-1"_s) {
	return Data::CallJoin::FromJson({
		{ u"call"_s, call },
		{ u"state"_s, state },
		{ u"sfu_url"_s, u"wss://sfu.test"_s },
		{ u"token"_s, token },
		{ u"peer"_s, u"u_sara"_s },
		{ u"relay_only"_s, true },
	});
}

[[nodiscard]] CallSignal Signal(
		const char *type,
		const QString &call,
		const QString &reason = QString(),
		const QString &from = u"u_sara"_s) {
	return CallSignal::FromJson(QString::fromLatin1(type), {
		{ u"t"_s, QString::fromLatin1(type) },
		{ u"call"_s, call },
		{ u"reason"_s, reason },
		{ u"from"_s, from },
	});
}

// Every request waits until the test answers it.
class FakeApi final : public Data::CallsApi {
public:
	struct JoinRequest {
		QString subject; // user id (invite) or call id
		base::Guard guard;
		JoinDone done;
		Data::FailCallback fail;
		bool answered = false;
	};
	struct GetRequest {
		QString call;
		base::Guard guard;
		Fn<void(const Data::CallInfo &)> done;
		Data::FailCallback fail;
		bool answered = false;
	};
	struct Command {
		QString call;
		QString verb;
		base::Guard guard;
		Done done;
		Data::FailCallback fail;
	};

	QVector<JoinRequest> invites;
	QVector<JoinRequest> accepts;
	QVector<JoinRequest> rejoins;
	QVector<GetRequest> gets;
	QVector<Command> commandsSent;
	QVector<Data::CallQos> reports;
	int incomingRequests = 0;
	int activeRequests = 0;
	Fn<void(const std::optional<Data::CallInfo> &)> incomingDone;
	Fn<void(const std::optional<Data::ActiveCall> &)> activeDone;
	bool online = true;
	// Commands answer by themselves (on the next turn of the loop).
	bool holdCommands = false;
	// When set, every poll is answered with this state at once.
	std::optional<Data::CallInfo> autoGet;
	int autoGetStatus = 0; // != 0: polls fail with this status

	void invite(const QString &userId, base::Guard guard, JoinDone done, Data::FailCallback fail) override {
		invites.push_back({ userId, guard, done, fail });
	}
	void accept(const QString &call, base::Guard guard, JoinDone done, Data::FailCallback fail) override {
		accepts.push_back({ call, guard, done, fail });
	}
	void rejoin(const QString &call, base::Guard guard, JoinDone done, Data::FailCallback fail) override {
		rejoins.push_back({ call, guard, done, fail });
	}
	void get(const QString &call, base::Guard guard, Fn<void(const Data::CallInfo &)> done, Data::FailCallback fail) override {
		gets.push_back({ call, guard, done, fail });
		if (autoGet || autoGetStatus) {
			const auto index = int(gets.size()) - 1;
			QTimer::singleShot(0, &_context, [=, this] {
				if (autoGetStatus) {
					failGet(index, autoGetStatus, u"call.not_found"_s);
				} else if (autoGet) {
					answerGet(index, *autoGet);
				}
			});
		}
	}
	void incoming(base::Guard guard, Fn<void(const std::optional<Data::CallInfo> &)> done, Data::FailCallback fail) override {
		Q_UNUSED(fail);
		++incomingRequests;
		incomingDone = [=](const std::optional<Data::CallInfo> &info) {
			if (guard.alive()) {
				done(info);
			}
		};
	}
	void active(base::Guard guard, Fn<void(const std::optional<Data::ActiveCall> &)> done, Data::FailCallback fail) override {
		Q_UNUSED(fail);
		++activeRequests;
		activeDone = [=](const std::optional<Data::ActiveCall> &call) {
			if (guard.alive()) {
				done(call);
			}
		};
	}
	void command(const QString &call, const QString &verb, base::Guard guard, Done done, Data::FailCallback fail) override {
		commandsSent.push_back({ call, verb, guard, done, fail });
		if (!holdCommands) {
			const auto index = int(commandsSent.size()) - 1;
			QTimer::singleShot(0, &_context, [=, this] { finishCommand(index); });
		}
	}
	void qos(const QString &call, const Data::CallQos &qos) override {
		Q_UNUSED(call);
		reports.push_back(qos);
	}
	bool signalsConnected() const override {
		return online;
	}

	void finishCommand(int index) {
		const auto command = commandsSent[index];
		if (command.guard.alive() && command.done) {
			command.done();
		}
	}
	void answer(QVector<JoinRequest> &list, int index, const Data::CallJoin &join) {
		auto &request = list[index];
		QVERIFY(!request.answered);
		request.answered = true;
		const auto copy = request;
		if (copy.guard.alive()) {
			copy.done(join);
		}
	}
	void fail(QVector<JoinRequest> &list, int index, int status, const QString &code) {
		auto &request = list[index];
		QVERIFY(!request.answered);
		request.answered = true;
		const auto copy = request;
		if (copy.guard.alive()) {
			copy.fail((status == 0)
				? Api::Error::Offline()
				: Api::Error::Local(status, code, code));
		}
	}
	void answerGet(int index, const Data::CallInfo &info) {
		auto &request = gets[index];
		if (request.answered) {
			return;
		}
		request.answered = true;
		const auto copy = request;
		if (copy.guard.alive()) {
			copy.done(info);
		}
	}
	void failGet(int index, int status, const QString &code) {
		auto &request = gets[index];
		if (request.answered) {
			return;
		}
		request.answered = true;
		const auto copy = request;
		if (copy.guard.alive()) {
			copy.fail(Api::Error::Local(status, code, code));
		}
	}

	// "cancel:ca1", "hangup:ca1", … in order.
	[[nodiscard]] QStringList commands() const {
		auto result = QStringList();
		for (const auto &command : commandsSent) {
			result.push_back(command.verb + u':' + command.call);
		}
		return result;
	}
	[[nodiscard]] int count(const QString &verb) const {
		return int(std::count_if(commandsSent.cbegin(), commandsSent.cend(), [&](
				const Command &command) {
			return command.verb == verb;
		}));
	}

private:
	QObject _context;

};

[[nodiscard]] Data::CallInfo Info(
		const QString &call,
		const QString &state,
		const QString &reason = QString(),
		qint64 duration = -1) {
	auto result = Data::CallInfo();
	result.call = call;
	result.state = state;
	result.endReason = reason;
	result.durationS = duration;
	result.peer = u"u_sara"_s;
	return result;
}

class FakeMedia final : public Calls::Media {
public:
	struct Log {
		QStringList joins; // tokens
		int leaves = 0;
		int statsRequests = 0;
		QVector<bool> mutes;
		QPointer<FakeMedia> media;
	};

	explicit FakeMedia(Log *log)
	: _log(log) {
		log->media = this;
	}

	void join(const Calls::MediaJoin &join) override {
		_log->joins.push_back(join.token);
		set(State::Connecting);
	}
	void leave() override {
		++_log->leaves;
		set(State::Idle);
	}
	State state() const override {
		return _state;
	}
	void setMuted(bool muted) override {
		_log->mutes.push_back(muted);
	}
	void setInputDevice(const QByteArray &) override {
	}
	void setOutputDevice(const QByteArray &) override {
	}
	void setProcessing(bool, bool, bool) override {
	}
	void requestStats() override {
		++_log->statsRequests;
	}

	void set(State state) {
		if (_state != state) {
			_state = state;
			Q_EMIT stateChanged(state);
		}
	}

private:
	Log *_log = nullptr;
	State _state = State::Idle;

};

[[nodiscard]] Calls::Timings FastTimings() {
	auto result = Calls::Timings();
	result.pollInCallMs = 80;
	result.pollConnectedMs = 40;
	result.pollOfflineMs = 15;
	result.ringTimeoutMs = 150;
	result.reconnectBudgetMs = 300;
	result.rejoinDelayMs = 20;
	result.qosFirstMs = 40;
	result.qosEachMs = 5000;
	return result;
}

struct Fixture {
	FakeApi api;
	FakeMedia::Log media;
	bool mediaAvailable = true;

	[[nodiscard]] Call::Args args(const QString &peer = u"u_sara"_s) {
		auto result = Call::Args();
		result.api = &api;
		result.createMedia = [this]() -> std::unique_ptr<Calls::Media> {
			if (!mediaAvailable) {
				return nullptr;
			}
			return std::make_unique<FakeMedia>(&media);
		};
		result.networkType = [] { return u"ethernet"_s; };
		result.peerId = peer;
		result.timings = FastTimings();
		return result;
	}
};

void Settle(int ms = 5) {
	QTest::qWait(ms);
}

} // namespace

class TestCallsCall : public QObject {
	Q_OBJECT

private Q_SLOTS:
	// ---- wire shapes
	void joinAndInfoFromJson();
	void signalFromFrame();
	void callRecords();
	void availability();
	void endReasons();

	// ---- outgoing
	void outgoingHappyPath();
	void outgoingAcceptedBeforeAudio();
	void outgoingSignalsBeforeTheAnswer();
	void outgoingEndBeforeItExists();
	void outgoingDestroyedBeforeItExists();
	void outgoingRefused_data();
	void outgoingRefused();
	void outgoingDeclinedAndUnanswered_data();
	void outgoingDeclinedAndUnanswered();
	void outgoingCancel();
	void outgoingRingsByPoll();
	void outgoingWithoutAudio();

	// ---- incoming
	void incomingAnswered();
	void incomingDeclined();
	void incomingCancelledByCaller();
	void incomingRingsOut();
	void incomingAnsweredElsewhereBySignal();
	void incomingAnsweredElsewhereByPoll();
	void incomingAcceptRefusedElsewhere();
	void incomingAcceptVersusCancel();
	void incomingAcceptVersusTimeout();
	void incomingAlreadyOver();

	// ---- in the call
	void duplicateSignals();
	void foreignSignalsIgnored();
	void endedByPeer();
	void endedSeenByPollOnly();
	void pollIsTheHeartbeat();
	void pollFasterWithoutSignals();
	void muteBeforeAndDuring();
	void libraryReconnectsByItself();
	void rejoinAfterDrop();
	void rejoinFindsTheCallOver();
	void rejoinRetriesThenGivesUp();
	void reconnectBudgetIsShared();
	void qualityReports();
	void destroyedInCallTellsTheServer();
	void exitInCall();
	void exitWhileRingingHere();
	void exitBeforeTheCallExists();
	void resumeAfterRestart();

	// ---- which call
	void instanceStartsOneCall();
	void instanceRingsAndIgnoresSecondCall();
	void instanceGlare();
	void instanceNoMicrophone();
	void instanceMediaMissing();
	void instanceIncomingCheckOncePerMinute();
	void instanceRestartRecovery_data();
	void instanceRestartRecovery();
	void instanceEndedCallStaysUntilDismissed();
	void instanceStoresTheCall();

};

void TestCallsCall::joinAndInfoFromJson() {
	const auto join = Data::CallJoin::FromJson(Json(R"({
		"call": "ca1", "state": "ringing", "sfu_url": "wss://sfu.eleza.test",
		"token": "jwt", "token_expires_at": 1800000300,
		"turn": { "username": "1800000300:x", "credential": "pw",
			"urls": ["turns:turn.eleza.test:443?transport=tcp", "http://not-a-turn", 7], "expires_at": 1800000300 },
		"peer": "u_sara", "relay_only": true })"));
	QVERIFY(join.valid());
	QCOMPARE(join.call, u"ca1"_s);
	QCOMPARE(join.state, u"ringing"_s);
	QCOMPARE(join.peer, u"u_sara"_s);
	const auto media = Calls::MediaJoin::FromJson(join.media);
	QVERIFY(media.valid());
	QCOMPARE(media.url, u"wss://sfu.eleza.test"_s);
	QCOMPARE(media.token, u"jwt"_s);
	QCOMPARE(media.turnUrls, QStringList{ u"turns:turn.eleza.test:443?transport=tcp"_s });
	QCOMPARE(media.turnUsername, u"1800000300:x"_s);
	QCOMPARE(media.turnCredential, u"pw"_s);
	QVERIFY(media.relayOnly);
	// No TURN in the answer: no credentials are passed on either.
	const auto bare = Calls::MediaJoin::FromJson(Json(R"({"sfu_url":"wss://a","token":"t","turn":{"username":"u","credential":"c","urls":[]}})"));
	QVERIFY(bare.turnUrls.isEmpty());
	QVERIFY(bare.turnUsername.isEmpty());
	QVERIFY(bare.relayOnly);
	QVERIFY(!Calls::MediaJoin::FromJson(Json(R"({"call":"ca1","state":"ended"})")).valid());

	const auto info = Data::CallInfo::FromJson(Json(R"({
		"call": "ca1", "state": "ended", "end_reason": "hangup", "peer": "u_sara",
		"outgoing": true, "created_at": 1800000000, "duration_s": 75 })"));
	QCOMPARE(info.state, u"ended"_s);
	QCOMPARE(info.endReason, u"hangup"_s);
	QVERIFY(info.outgoing);
	QCOMPARE(info.durationS, qint64(75));
	const auto ringing = Data::CallInfo::FromJson(Json(R"({"call":"ca1","state":"ringing","peer":"u_sara","outgoing":true,"delivered":true})"));
	QVERIFY(ringing.delivered);
	QCOMPARE(ringing.durationS, qint64(-1));
	QVERIFY(!Data::CallInfo::FromJson(QJsonObject()).valid());

	const auto active = Data::ActiveCall::FromJson(Json(R"({"call":"ca9","state":"connected","peer":"u_ali","role":"caller","created_at":5})"));
	QCOMPARE(active.call, u"ca9"_s);
	QVERIFY(active.caller);
	QVERIFY(!Data::ActiveCall::FromJson(Json(R"({"call":"ca9","role":"callee"})")).caller);

	auto qos = Data::CallQos();
	qos.rttMs = 42.5;
	qos.relayTcp = true;
	qos.durationMs = 61000;
	const auto body = qos.toJson();
	QCOMPARE(body.value(u"rtt_ms"_s).toDouble(), 42.5);
	QCOMPARE(body.value(u"net_type"_s).toString(), u"unknown"_s);
	QCOMPARE(body.value(u"relay_tcp"_s).toBool(), true);
	QCOMPARE(body.value(u"duration_ms"_s).toDouble(), 61000.);
	QVERIFY(body.contains(u"jitter_ms"_s) && body.contains(u"loss_pct"_s) && body.contains(u"mos"_s));
}

void TestCallsCall::signalFromFrame() {
	auto frame = Rt::Frame();
	frame.type = Rt::Frame::Type::Call;
	frame.kind = u"call.incoming"_s;
	frame.data = Json(R"({"t":"call.incoming","call":"ca1","state":"ringing","reason":"","from":"u_sara"})");
	const auto incoming = CallSignal::FromFrame(frame);
	QCOMPARE(incoming.type, CallSignal::Type::Incoming);
	QCOMPARE(incoming.call, u"ca1"_s);
	QCOMPARE(incoming.from, u"u_sara"_s);

	frame.kind = u"call.answered_elsewhere"_s;
	frame.data = Json(R"({"call":"ca1","state":"accepted","from":"u_sara","device":"d2"})");
	const auto elsewhere = CallSignal::FromFrame(frame);
	QCOMPARE(elsewhere.type, CallSignal::Type::AnsweredElsewhere);
	QCOMPARE(elsewhere.device, u"d2"_s);

	frame.kind = u"call.ended"_s;
	frame.data = Json(R"({"call":"ca1","state":"ended","reason":"rejected","from":"u_sara"})");
	QCOMPARE(CallSignal::FromFrame(frame).type, CallSignal::Type::Ended);
	QCOMPARE(CallSignal::FromFrame(frame).reason, u"rejected"_s);

	QCOMPARE(CallSignal::FromJson(u"call.ringing"_s, Json(R"({"call":"ca1"})")).type, CallSignal::Type::Ringing);
	QCOMPARE(CallSignal::FromJson(u"call.accepted"_s, Json(R"({"call":"ca1"})")).type, CallSignal::Type::Accepted);
	// A kind this build does not know, a signal without a call, another frame.
	QCOMPARE(CallSignal::FromJson(u"call.video_offer"_s, Json(R"({"call":"ca1"})")).type, CallSignal::Type::Unknown);
	QCOMPARE(CallSignal::FromJson(u"call.ended"_s, QJsonObject()).type, CallSignal::Type::Unknown);
	frame.type = Rt::Frame::Type::Typing;
	QCOMPARE(CallSignal::FromFrame(frame).type, CallSignal::Type::Unknown);
}

void TestCallsCall::callRecords() {
	using Data::CallRecordKind;
	const auto record = [](const char *outcome, qint64 duration) {
		auto result = Data::CallRecord();
		result.outcome = QString::fromLatin1(outcome);
		result.durationS = duration;
		return result;
	};
	QCOMPARE(Data::ClassifyCallRecord(record("completed", 75), true), CallRecordKind::Outgoing);
	QCOMPARE(Data::ClassifyCallRecord(record("completed", 75), false), CallRecordKind::Incoming);
	// Connected for less than a second is still a call that happened.
	QCOMPARE(Data::ClassifyCallRecord(record("completed", 0), false), CallRecordKind::Incoming);
	QCOMPARE(Data::ClassifyCallRecord(record("missed", 0), true), CallRecordKind::OutgoingNoAnswer);
	QCOMPARE(Data::ClassifyCallRecord(record("missed", 0), false), CallRecordKind::Missed);
	QCOMPARE(Data::ClassifyCallRecord(record("declined", 0), true), CallRecordKind::OutgoingDeclined);
	QCOMPARE(Data::ClassifyCallRecord(record("declined", 0), false), CallRecordKind::IncomingDeclined);
	// What older servers wrote, and an outcome nobody knows yet.
	QCOMPARE(Data::ClassifyCallRecord(record("rejected", 0), false), CallRecordKind::IncomingDeclined);
	QCOMPARE(Data::ClassifyCallRecord(record("answered", 0), true), CallRecordKind::Outgoing);
	QCOMPARE(Data::ClassifyCallRecord(record("whatever", 0), false), CallRecordKind::Missed);
	QVERIFY(Data::CallRecordIsMissed(CallRecordKind::Missed));
	QVERIFY(Data::CallRecordIsMissed(CallRecordKind::IncomingDeclined));
	QVERIFY(!Data::CallRecordIsMissed(CallRecordKind::OutgoingNoAnswer));
	QVERIFY(!Data::CallRecordIsMissed(CallRecordKind::OutgoingDeclined));
	QVERIFY(!Data::CallRecordIsMissed(CallRecordKind::Incoming));
}

void TestCallsCall::availability() {
	using Data::CallAvailability;
	auto chat = Data::Chat();
	chat.id = u"c1"_s;
	chat.peerUserId = u"u_sara"_s;
	chat.state = u"active"_s;
	QCOMPARE(Data::CallAvailabilityFor(chat), CallAvailability::Available);
	{
		auto copy = chat;
		copy.state = QString(); // an old cache row
		QCOMPARE(Data::CallAvailabilityFor(copy), CallAvailability::Available);
	}
	const auto with = [&](Fn<void(Data::Chat &)> change) {
		auto copy = chat;
		change(copy);
		return Data::CallAvailabilityFor(copy);
	};
	// Never a chat with calls.
	QCOMPARE(with([](Data::Chat &c) { c.isConversation = true; }), CallAvailability::Hidden);
	QCOMPARE(with([](Data::Chat &c) { c.anonymous = true; }), CallAvailability::Hidden);
	QCOMPARE(with([](Data::Chat &c) { c.isBot = true; }), CallAvailability::Hidden);
	QCOMPARE(with([](Data::Chat &c) { c.saved = true; }), CallAvailability::Hidden);
	QCOMPARE(with([](Data::Chat &c) { c.peerDeleted = true; }), CallAvailability::Hidden);
	QCOMPARE(with([](Data::Chat &c) { c.peerUserId = QString(); }), CallAvailability::Hidden);
	// A direct chat the server refuses calls in right now.
	QCOMPARE(with([](Data::Chat &c) { c.section = u"request"_s; }), CallAvailability::Disabled);
	QCOMPARE(with([](Data::Chat &c) { c.request = u"outgoing"_s; }), CallAvailability::Disabled);
	QCOMPARE(with([](Data::Chat &c) { c.state = u"request"_s; }), CallAvailability::Disabled);
	QCOMPARE(with([](Data::Chat &c) { c.state = u"blocked"_s; }), CallAvailability::Disabled);
	QCOMPARE(with([](Data::Chat &c) { c.canSend = false; }), CallAvailability::Disabled);
}

void TestCallsCall::endReasons() {
	QCOMPARE(Calls::EndReasonFromServer(u"rejected"_s), EndReason::Declined);
	QCOMPARE(Calls::EndReasonFromServer(u"timeout"_s), EndReason::NoAnswer);
	QCOMPARE(Calls::EndReasonFromServer(u"lost"_s), EndReason::Lost);
	QCOMPARE(Calls::EndReasonFromServer(u"network"_s), EndReason::Lost);
	QCOMPARE(Calls::EndReasonFromServer(u"hangup"_s), EndReason::Hangup);
	QCOMPARE(Calls::EndReasonFromServer(u"cancelled"_s), EndReason::Hangup);
	QCOMPARE(Calls::EndReasonFromServer(u"max_duration"_s), EndReason::Hangup);
	QCOMPARE(Calls::EndReasonFromServer(u"blocked"_s), EndReason::Hangup);
	QCOMPARE(Calls::EndReasonFromServer(u"something_new"_s), EndReason::Hangup);
	QCOMPARE(Calls::EndReasonFromServer(QString()), EndReason::Hangup);
	QCOMPARE(Calls::EndReasonFromStartError(Api::Error::Local(409, u"call.busy"_s, u"busy"_s)), EndReason::Busy);
	QCOMPARE(Calls::EndReasonFromStartError(Api::Error::Local(403, u"call.not_allowed"_s, u"no"_s)), EndReason::NotAllowed);
	QCOMPARE(Calls::EndReasonFromStartError(Api::Error::Local(503, u"call.capacity"_s, u"full"_s)), EndReason::Failed);
	QCOMPARE(Calls::EndReasonFromStartError(Api::Error::Offline()), EndReason::Failed);
}

// ---- outgoing

void TestCallsCall::outgoingHappyPath() {
	auto f = Fixture();
	auto call = Call(f.args());
	auto states = QVector<State>();
	connect(&call, &Call::stateChanged, [&](State state) { states.push_back(state); });
	call.start();
	QCOMPARE(call.state(), State::Calling);
	QVERIFY(call.outgoing());
	QCOMPARE(f.api.invites.size(), 1);
	QCOMPARE(f.api.invites[0].subject, u"u_sara"_s);
	QVERIFY(call.id().isEmpty());

	f.api.answer(f.api.invites, 0, Join(u"ca1"_s));
	QCOMPARE(call.id(), u"ca1"_s);
	// The caller joins the audio room while it still rings.
	QCOMPARE(f.media.joins, QStringList{ u"token-1"_s });
	QCOMPARE(call.state(), State::Calling);

	call.handleSignal(Signal("call.ringing", u"ca1"_s));
	QCOMPARE(call.state(), State::Ringing);

	// My audio is ready before they answer: nothing is reported yet.
	f.media.media->set(Calls::Media::State::Connected);
	QCOMPARE(call.state(), State::Ringing);
	Settle();
	QCOMPARE(f.api.count(u"connected"_s), 0);

	call.handleSignal(Signal("call.accepted", u"ca1"_s));
	QCOMPARE(call.state(), State::InCall);
	QVERIFY(call.connectedAt() > 0);
	QCOMPARE(f.api.commands(), QStringList{ u"connected:ca1"_s });

	call.hangup();
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(call.endReason(), EndReason::Hangup);
	QVERIFY(!call.quiet());
	QVERIFY(call.duration() >= 0);
	QCOMPARE(f.api.commands(), (QStringList{ u"connected:ca1"_s, u"hangup:ca1"_s }));
	QCOMPARE(f.media.leaves, 1);
	QCOMPARE(states, (QVector<State>{ State::Ringing, State::Connecting, State::InCall, State::Ended }));

	// Nothing moves a call that is over.
	call.handleSignal(Signal("call.accepted", u"ca1"_s));
	call.hangup();
	Settle(100);
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(f.api.count(u"hangup"_s), 1);
}

void TestCallsCall::outgoingAcceptedBeforeAudio() {
	auto f = Fixture();
	auto call = Call(f.args());
	call.start();
	f.api.answer(f.api.invites, 0, Join(u"ca1"_s));
	call.handleSignal(Signal("call.accepted", u"ca1"_s));
	QCOMPARE(call.state(), State::Connecting);
	QCOMPARE(f.api.count(u"connected"_s), 0);
	f.media.media->set(Calls::Media::State::Connected);
	QCOMPARE(call.state(), State::InCall);
	QCOMPARE(f.api.count(u"connected"_s), 1);
}

void TestCallsCall::outgoingSignalsBeforeTheAnswer() {
	// The server signals faster than it answers the request that created
	// the call: ringing and accepted arrive while its id is unknown here.
	auto f = Fixture();
	auto call = Call(f.args());
	call.start();
	call.handleSignal(Signal("call.ringing", u"ca1"_s));
	call.handleSignal(Signal("call.ended", u"other"_s, u"hangup"_s)); // not mine
	call.handleSignal(Signal("call.accepted", u"ca1"_s));
	QCOMPARE(call.state(), State::Calling);
	f.api.answer(f.api.invites, 0, Join(u"ca1"_s));
	QCOMPARE(call.state(), State::Connecting);
	f.media.media->set(Calls::Media::State::Connected);
	QCOMPARE(call.state(), State::InCall);

	// The same with the end: they declined before the answer arrived.
	auto g = Fixture();
	auto second = Call(g.args());
	second.start();
	second.handleSignal(Signal("call.ended", u"ca2"_s, u"rejected"_s));
	g.api.answer(g.api.invites, 0, Join(u"ca2"_s));
	QCOMPARE(second.state(), State::Ended);
	QCOMPARE(second.endReason(), EndReason::Declined);
	Settle();
	QCOMPARE(g.api.count(u"cancel"_s), 0); // the server ended it
}

void TestCallsCall::outgoingEndBeforeItExists() {
	auto f = Fixture();
	auto call = Call(f.args());
	call.start();
	call.hangup();
	QCOMPARE(call.state(), State::Ended);
	QVERIFY(call.quiet());
	QVERIFY(f.api.commandsSent.isEmpty());
	// The call it created is cancelled as soon as it has a name.
	f.api.answer(f.api.invites, 0, Join(u"ca1"_s));
	QCOMPARE(f.api.commands(), QStringList{ u"cancel:ca1"_s });
	QVERIFY(f.media.joins.isEmpty());
	QVERIFY(call.id().isEmpty());
}

void TestCallsCall::outgoingDestroyedBeforeItExists() {
	auto f = Fixture();
	{
		auto call = Call(f.args());
		call.start();
	}
	QVERIFY(f.api.commandsSent.isEmpty());
	f.api.answer(f.api.invites, 0, Join(u"ca1"_s));
	QCOMPARE(f.api.commands(), QStringList{ u"cancel:ca1"_s });

	// Refused meanwhile: nothing to cancel, nothing crashes.
	auto g = Fixture();
	{
		auto call = Call(g.args());
		call.start();
		call.hangup();
	}
	g.api.fail(g.api.invites, 0, 409, u"call.busy"_s);
	QVERIFY(g.api.commandsSent.isEmpty());
}

void TestCallsCall::outgoingRefused_data() {
	QTest::addColumn<int>("status");
	QTest::addColumn<QString>("code");
	QTest::addColumn<int>("reason");
	QTest::newRow("busy") << 409 << u"call.busy"_s << int(EndReason::Busy);
	QTest::newRow("privacy / blocked / no chat") << 403 << u"call.not_allowed"_s << int(EndReason::NotAllowed);
	QTest::newRow("capacity") << 503 << u"call.capacity"_s << int(EndReason::Failed);
	QTest::newRow("rate limited") << 429 << u"rate_limited"_s << int(EndReason::Failed);
	QTest::newRow("offline") << 0 << u"offline"_s << int(EndReason::Failed);
}

void TestCallsCall::outgoingRefused() {
	QFETCH(int, status);
	QFETCH(QString, code);
	QFETCH(int, reason);
	auto f = Fixture();
	auto call = Call(f.args());
	call.start();
	f.api.fail(f.api.invites, 0, status, code);
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(int(call.endReason()), reason);
	QVERIFY(!call.quiet()); // shown: the user asked for this call
	QVERIFY(call.startError().has_value());
	QCOMPARE(call.duration(), qint64(-1));
	// There is no call to end.
	call.hangup();
	Settle();
	QVERIFY(f.api.commandsSent.isEmpty());
	QVERIFY(f.media.joins.isEmpty());
}

void TestCallsCall::outgoingDeclinedAndUnanswered_data() {
	QTest::addColumn<QString>("serverReason");
	QTest::addColumn<int>("reason");
	QTest::newRow("declined") << u"rejected"_s << int(EndReason::Declined);
	QTest::newRow("rang out") << u"timeout"_s << int(EndReason::NoAnswer);
	QTest::newRow("blocked meanwhile") << u"blocked"_s << int(EndReason::Hangup);
}

void TestCallsCall::outgoingDeclinedAndUnanswered() {
	QFETCH(QString, serverReason);
	QFETCH(int, reason);
	auto f = Fixture();
	auto call = Call(f.args());
	call.start();
	f.api.answer(f.api.invites, 0, Join(u"ca1"_s));
	call.handleSignal(Signal("call.ringing", u"ca1"_s));
	call.handleSignal(Signal("call.ended", u"ca1"_s, serverReason));
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(int(call.endReason()), reason);
	QVERIFY(!call.quiet());
	QCOMPARE(call.duration(), qint64(-1));
	QCOMPARE(f.media.leaves, 1);
	Settle();
	// The server ended it: this side tells nothing.
	QVERIFY(f.api.commandsSent.isEmpty());
}

void TestCallsCall::outgoingCancel() {
	auto f = Fixture();
	auto call = Call(f.args());
	call.start();
	f.api.answer(f.api.invites, 0, Join(u"ca1"_s));
	call.handleSignal(Signal("call.ringing", u"ca1"_s));
	call.hangup();
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(f.api.commands(), QStringList{ u"cancel:ca1"_s });
	// The echo of my own cancel changes nothing.
	call.handleSignal(Signal("call.ended", u"ca1"_s, u"cancelled"_s));
	QCOMPARE(call.endReason(), EndReason::Hangup);
	QCOMPARE(f.api.commandsSent.size(), 1);
}

void TestCallsCall::outgoingRingsByPoll() {
	// Without the realtime connection the poll is what moves the screen.
	auto f = Fixture();
	f.api.online = false;
	auto call = Call(f.args());
	call.start();
	f.api.answer(f.api.invites, 0, Join(u"ca1"_s));
	QCOMPARE(call.state(), State::Calling);
	QTRY_VERIFY(!f.api.gets.isEmpty());
	auto info = Info(u"ca1"_s, u"ringing"_s);
	info.delivered = true;
	f.api.answerGet(0, info);
	QCOMPARE(call.state(), State::Ringing);
	QTRY_VERIFY(f.api.gets.size() >= 2);
	f.api.answerGet(int(f.api.gets.size()) - 1, Info(u"ca1"_s, u"accepted"_s));
	QCOMPARE(call.state(), State::Connecting);
	f.media.media->set(Calls::Media::State::Connected);
	QCOMPARE(call.state(), State::InCall);
	// "connected" from the other side's report, mine already up.
	QTRY_VERIFY(f.api.gets.size() >= 3);
	f.api.answerGet(int(f.api.gets.size()) - 1, Info(u"ca1"_s, u"connected"_s));
	QCOMPARE(call.state(), State::InCall);
	QCOMPARE(f.api.count(u"connected"_s), 1);
}

void TestCallsCall::outgoingWithoutAudio() {
	// The media library is missing: a call nobody can hear is ended at once.
	auto f = Fixture();
	f.mediaAvailable = false;
	auto call = Call(f.args());
	call.start();
	f.api.answer(f.api.invites, 0, Join(u"ca1"_s));
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(call.endReason(), EndReason::Failed);
	QCOMPARE(f.api.commands(), QStringList{ u"cancel:ca1"_s });
}

// ---- incoming

void TestCallsCall::incomingAnswered() {
	auto f = Fixture();
	auto call = Call(f.args());
	call.ring(u"ca1"_s);
	QCOMPARE(call.state(), State::Incoming);
	QVERIFY(!call.outgoing());
	// The caller's screen learns that it rings here; the state is asked at once.
	QCOMPARE(f.api.commands(), QStringList{ u"ringing:ca1"_s });
	QCOMPARE(f.api.gets.size(), 1);
	f.api.answerGet(0, Info(u"ca1"_s, u"ringing"_s));
	QCOMPARE(call.state(), State::Incoming);

	call.answer();
	QCOMPARE(call.state(), State::Connecting);
	QCOMPARE(f.api.accepts.size(), 1);
	QVERIFY(f.media.joins.isEmpty()); // not before the server let me in
	// Sent to every device of mine, this one included.
	call.handleSignal(Signal("call.answered_elsewhere", u"ca1"_s));
	QCOMPARE(call.state(), State::Connecting);

	f.api.answer(f.api.accepts, 0, Join(u"ca1"_s, u"accepted"_s));
	QCOMPARE(f.media.joins.size(), 1);
	QCOMPARE(call.state(), State::Connecting);
	f.media.media->set(Calls::Media::State::Connected);
	QCOMPARE(call.state(), State::InCall);
	QCOMPARE(f.api.commands(), (QStringList{ u"ringing:ca1"_s, u"connected:ca1"_s }));

	// It does not ring out once answered.
	Settle(200);
	QCOMPARE(call.state(), State::InCall);
	call.answer(); // a second Enter
	QCOMPARE(f.api.accepts.size(), 1);

	call.hangup();
	QCOMPARE(f.api.commands().last(), u"hangup:ca1"_s);
	QCOMPARE(call.endReason(), EndReason::Hangup);
	QVERIFY(!call.quiet());
}

void TestCallsCall::incomingDeclined() {
	auto f = Fixture();
	auto call = Call(f.args());
	call.ring(u"ca1"_s);
	call.hangup();
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(call.endReason(), EndReason::DeclinedByMe);
	QVERIFY(call.quiet());
	QCOMPARE(f.api.commands(), (QStringList{ u"ringing:ca1"_s, u"reject:ca1"_s }));
	QVERIFY(f.media.joins.isEmpty());
}

void TestCallsCall::incomingCancelledByCaller() {
	auto f = Fixture();
	auto call = Call(f.args());
	call.ring(u"ca1"_s);
	call.handleSignal(Signal("call.ended", u"ca1"_s, u"cancelled"_s));
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(call.endReason(), EndReason::Missed);
	QVERIFY(call.quiet());
	Settle();
	QCOMPARE(f.api.commands(), QStringList{ u"ringing:ca1"_s });
}

void TestCallsCall::incomingRingsOut() {
	// Offline and nothing says the call is over: it stops ringing by itself,
	// and tells the server nothing (other devices of mine may still answer).
	auto f = Fixture();
	auto call = Call(f.args());
	call.ring(u"ca1"_s);
	QTRY_COMPARE_WITH_TIMEOUT(call.state(), State::Ended, 10000);
	QCOMPARE(call.endReason(), EndReason::Missed);
	QVERIFY(call.quiet());
	QCOMPARE(f.api.commands(), QStringList{ u"ringing:ca1"_s });
}

void TestCallsCall::incomingAnsweredElsewhereBySignal() {
	auto f = Fixture();
	auto call = Call(f.args());
	call.ring(u"ca1"_s);
	call.handleSignal(Signal("call.answered_elsewhere", u"ca1"_s));
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(call.endReason(), EndReason::AnsweredElsewhere);
	QVERIFY(call.quiet());
	// That call is somebody else's now: it must not be ended from here.
	call.hangup();
	Settle();
	QCOMPARE(f.api.commands(), QStringList{ u"ringing:ca1"_s });
}

void TestCallsCall::incomingAnsweredElsewhereByPoll() {
	auto f = Fixture();
	auto call = Call(f.args());
	call.ring(u"ca1"_s);
	f.api.answerGet(0, Info(u"ca1"_s, u"accepted"_s));
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(call.endReason(), EndReason::AnsweredElsewhere);
	QVERIFY(call.quiet());

	auto g = Fixture();
	auto second = Call(g.args());
	second.ring(u"ca2"_s);
	second.handleSignal(Signal("call.accepted", u"ca2"_s));
	QCOMPARE(second.endReason(), EndReason::AnsweredElsewhere);
}

void TestCallsCall::incomingAcceptRefusedElsewhere() {
	// Two devices pressed Accept; the other one won.
	auto f = Fixture();
	auto call = Call(f.args());
	call.ring(u"ca1"_s);
	call.answer();
	f.api.fail(f.api.accepts, 0, 409, u"call.answered_elsewhere"_s);
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(call.endReason(), EndReason::AnsweredElsewhere);
	QVERIFY(call.quiet());
	Settle();
	QCOMPARE(f.api.count(u"hangup"_s), 0);
	QCOMPARE(f.api.count(u"reject"_s), 0);
	QVERIFY(f.media.joins.isEmpty());
}

void TestCallsCall::incomingAcceptVersusCancel() {
	// Accept is pressed in the moment the caller gives up. Whichever the
	// server saw first wins; here the cancel.
	auto f = Fixture();
	auto call = Call(f.args());
	call.ring(u"ca1"_s);
	call.answer();
	QCOMPARE(call.state(), State::Connecting);
	call.handleSignal(Signal("call.ended", u"ca1"_s, u"cancelled"_s));
	QCOMPARE(call.state(), State::Ended);
	QVERIFY(!call.quiet()); // the user is looking at «connecting»: say that it ended
	f.api.fail(f.api.accepts, 0, 409, u"call.state"_s);
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(call.endReason(), EndReason::Hangup);
	Settle();
	QCOMPARE(f.api.count(u"hangup"_s), 0);
	QVERIFY(f.media.joins.isEmpty());

	// The other order: the refusal arrives first.
	auto g = Fixture();
	auto second = Call(g.args());
	second.ring(u"ca2"_s);
	second.answer();
	g.api.fail(g.api.accepts, 0, 409, u"call.state"_s);
	QCOMPARE(second.state(), State::Ended);
	QCOMPARE(second.endReason(), EndReason::Missed);
	QVERIFY(second.quiet());
	second.handleSignal(Signal("call.ended", u"ca2"_s, u"cancelled"_s));
	QCOMPARE(second.endReason(), EndReason::Missed);

	// And the accept that wins against a cancel that comes too late.
	auto h = Fixture();
	auto third = Call(h.args());
	third.ring(u"ca3"_s);
	third.answer();
	h.api.answer(h.api.accepts, 0, Join(u"ca3"_s, u"accepted"_s));
	h.media.media->set(Calls::Media::State::Connected);
	QCOMPARE(third.state(), State::InCall);
}

void TestCallsCall::incomingAcceptVersusTimeout() {
	// Accept right when the ring times out on the server.
	auto f = Fixture();
	auto call = Call(f.args());
	call.ring(u"ca1"_s);
	call.answer();
	f.api.fail(f.api.accepts, 0, 404, u"call.not_found"_s);
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(call.endReason(), EndReason::Missed);
	QVERIFY(call.quiet());
	// The local ring timer is not running any more either.
	Settle(250);
	QCOMPARE(call.endReason(), EndReason::Missed);
	QCOMPARE(f.api.count(u"reject"_s), 0);
}

void TestCallsCall::incomingAlreadyOver() {
	// The signal was late: the first look at the call says it has ended.
	auto f = Fixture();
	auto call = Call(f.args());
	call.ring(u"ca1"_s);
	f.api.answerGet(0, Info(u"ca1"_s, u"ended"_s, u"timeout"_s));
	QCOMPARE(call.state(), State::Ended);
	QVERIFY(call.quiet());

	auto g = Fixture();
	auto gone = Call(g.args());
	gone.ring(u"ca2"_s);
	g.api.failGet(0, 404, u"call.not_found"_s);
	QCOMPARE(gone.state(), State::Ended);
	QVERIFY(gone.quiet());
}

// ---- in the call

namespace {

// An outgoing call that both sides are talking in.
void Connect(Fixture &f, Call &call, const QString &id = u"ca1"_s) {
	call.start();
	f.api.answer(f.api.invites, 0, Join(id));
	call.handleSignal(Signal("call.accepted", id));
	f.media.media->set(Calls::Media::State::Connected);
	QCOMPARE(call.state(), State::InCall);
}

} // namespace

void TestCallsCall::duplicateSignals() {
	auto f = Fixture();
	auto call = Call(f.args());
	auto states = QVector<State>();
	connect(&call, &Call::stateChanged, [&](State state) { states.push_back(state); });
	call.start();
	f.api.answer(f.api.invites, 0, Join(u"ca1"_s));
	for (auto i = 0; i != 3; ++i) {
		call.handleSignal(Signal("call.ringing", u"ca1"_s));
	}
	for (auto i = 0; i != 3; ++i) {
		call.handleSignal(Signal("call.accepted", u"ca1"_s));
	}
	f.media.media->set(Calls::Media::State::Connected);
	call.handleSignal(Signal("call.accepted", u"ca1"_s));
	call.handleSignal(Signal("call.ringing", u"ca1"_s));
	for (auto i = 0; i != 3; ++i) {
		call.handleSignal(Signal("call.ended", u"ca1"_s, u"hangup"_s));
	}
	QCOMPARE(states, (QVector<State>{ State::Ringing, State::Connecting, State::InCall, State::Ended }));
	Settle();
	QCOMPARE(f.api.commands(), QStringList{ u"connected:ca1"_s });
	QCOMPARE(f.media.leaves, 1);
}

void TestCallsCall::foreignSignalsIgnored() {
	auto f = Fixture();
	auto call = Call(f.args());
	Connect(f, call);
	call.handleSignal(Signal("call.ended", u"ca2"_s, u"hangup"_s));
	call.handleSignal(Signal("call.answered_elsewhere", u"ca2"_s));
	call.handleSignal(Signal("call.incoming", u"ca3"_s));
	call.handleSignal(CallSignal());
	QCOMPARE(call.state(), State::InCall);
}

void TestCallsCall::endedByPeer() {
	auto f = Fixture();
	auto call = Call(f.args());
	Connect(f, call);
	call.handleSignal(Signal("call.ended", u"ca1"_s, u"hangup"_s));
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(call.endReason(), EndReason::Hangup);
	QVERIFY(call.duration() >= 0);
	Settle();
	QCOMPARE(f.api.count(u"hangup"_s), 0);
	QCOMPARE(f.media.leaves, 1);
	// The room is closed with the call: its "failed" asks for nothing.
	f.media.media->set(Calls::Media::State::Failed);
	Settle(50);
	QVERIFY(f.api.rejoins.isEmpty());
}

void TestCallsCall::endedSeenByPollOnly() {
	// The "ended" signal was lost; the poll brings the end and the duration.
	auto f = Fixture();
	auto call = Call(f.args());
	Connect(f, call);
	QTRY_VERIFY(!f.api.gets.isEmpty());
	f.api.answerGet(int(f.api.gets.size()) - 1, Info(u"ca1"_s, u"ended"_s, u"lost"_s, 42));
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(call.endReason(), EndReason::Lost);
	QCOMPARE(call.duration(), qint64(42));

	auto g = Fixture();
	auto gone = Call(g.args());
	Connect(g, gone, u"ca2"_s);
	QTRY_VERIFY(!g.api.gets.isEmpty());
	g.api.failGet(int(g.api.gets.size()) - 1, 404, u"call.not_found"_s);
	QCOMPARE(gone.state(), State::Ended);
	// A failed poll that is not "gone" ends nothing.
	auto h = Fixture();
	auto alive = Call(h.args());
	Connect(h, alive, u"ca3"_s);
	QTRY_VERIFY(!h.api.gets.isEmpty());
	h.api.failGet(int(h.api.gets.size()) - 1, 503, u"server.overloaded"_s);
	QCOMPARE(alive.state(), State::InCall);
}

void TestCallsCall::pollIsTheHeartbeat() {
	auto f = Fixture();
	f.api.autoGet = Info(u"ca1"_s, u"connected"_s);
	auto call = Call(f.args());
	Connect(f, call);
	const auto before = f.api.gets.size();
	// 80 ms in the call: about five polls in 400 ms, never a burst (a busy
	// machine may make it fewer, never more).
	Settle(400);
	const auto polls = f.api.gets.size() - before;
	QVERIFY2(polls >= 1 && polls <= 7, qPrintable(QString::number(polls)));
	call.hangup();
	const auto after = f.api.gets.size();
	Settle(250);
	QCOMPARE(f.api.gets.size(), after); // and none once it is over
}

void TestCallsCall::pollFasterWithoutSignals() {
	auto f = Fixture();
	f.api.autoGet = Info(u"ca1"_s, u"ringing"_s);
	auto call = Call(f.args());
	call.start();
	f.api.answer(f.api.invites, 0, Join(u"ca1"_s));
	// Connected: every 40 ms.
	Settle(300);
	const auto slow = f.api.gets.size();
	QVERIFY2(slow >= 1 && slow <= 9, qPrintable(QString::number(slow)));
	// The connection drops: asked at once, then every 15 ms.
	f.api.online = false;
	call.signalsConnectedChanged(false);
	QCOMPARE(f.api.gets.size(), slow + 1);
	QTRY_VERIFY_WITH_TIMEOUT(f.api.gets.size() - slow >= 10, 3000);
	// Back: asked at once (signals may have been missed), slower again.
	f.api.online = true;
	const auto before = f.api.gets.size();
	call.signalsConnectedChanged(true);
	QCOMPARE(f.api.gets.size(), before + 1);
}

void TestCallsCall::muteBeforeAndDuring() {
	auto f = Fixture();
	auto call = Call(f.args());
	auto changes = 0;
	connect(&call, &Call::mutedChanged, [&] { ++changes; });
	call.start();
	call.setMuted(true); // while it is still being set up
	call.setMuted(true);
	QCOMPARE(changes, 1);
	f.api.answer(f.api.invites, 0, Join(u"ca1"_s));
	QCOMPARE(f.media.mutes, QVector<bool>{ true });
	call.setMuted(false);
	QCOMPARE(f.media.mutes, (QVector<bool>{ true, false }));
	QVERIFY(!call.muted());
	QCOMPARE(changes, 2);
}

void TestCallsCall::libraryReconnectsByItself() {
	// A network change the library recovers from: no new credentials.
	auto f = Fixture();
	auto call = Call(f.args());
	Connect(f, call);
	f.media.media->set(Calls::Media::State::Reconnecting);
	QCOMPARE(call.state(), State::Reconnecting);
	f.media.media->set(Calls::Media::State::Connected);
	QCOMPARE(call.state(), State::InCall);
	QVERIFY(f.api.rejoins.isEmpty());
	QCOMPARE(f.api.count(u"connected"_s), 1);
	// The budget was returned: nothing fails later.
	Settle(400);
	QCOMPARE(call.state(), State::InCall);

	// "Reconnecting" means nothing for a call that still rings there.
	auto g = Fixture();
	auto ringing = Call(g.args());
	ringing.start();
	g.api.answer(g.api.invites, 0, Join(u"ca2"_s));
	g.media.media->set(Calls::Media::State::Connected);
	g.media.media->set(Calls::Media::State::Reconnecting);
	QCOMPARE(ringing.state(), State::Calling);
}

void TestCallsCall::rejoinAfterDrop() {
	auto f = Fixture();
	auto call = Call(f.args());
	Connect(f, call);
	f.media.media->set(Calls::Media::State::Failed);
	QCOMPARE(call.state(), State::Reconnecting);
	QCOMPARE(f.api.rejoins.size(), 1);
	QCOMPARE(f.api.rejoins[0].subject, u"ca1"_s);
	// One request at a time, whatever the media repeats.
	f.media.media->set(Calls::Media::State::Connecting);
	f.media.media->set(Calls::Media::State::Failed);
	QCOMPARE(f.api.rejoins.size(), 1);
	f.api.answer(f.api.rejoins, 0, Join(u"ca1"_s, u"connected"_s, u"token-2"_s));
	QCOMPARE(f.media.joins, (QStringList{ u"token-1"_s, u"token-2"_s }));
	QCOMPARE(call.state(), State::Reconnecting);
	f.media.media->set(Calls::Media::State::Connected);
	QCOMPARE(call.state(), State::InCall);
	QCOMPARE(f.api.count(u"connected"_s), 1); // reported once per call
	Settle(400);
	QCOMPARE(call.state(), State::InCall);

	// The caller's room drops while it still rings there: joined again
	// behind the ringing screen.
	auto g = Fixture();
	auto ringing = Call(g.args());
	ringing.start();
	g.api.answer(g.api.invites, 0, Join(u"ca2"_s));
	ringing.handleSignal(Signal("call.ringing", u"ca2"_s));
	g.media.media->set(Calls::Media::State::Failed);
	QCOMPARE(ringing.state(), State::Ringing);
	QCOMPARE(g.api.rejoins.size(), 1);
	g.api.answer(g.api.rejoins, 0, Join(u"ca2"_s, u"ringing"_s, u"token-2"_s));
	QCOMPARE(g.media.joins.size(), 2);
}

void TestCallsCall::rejoinFindsTheCallOver() {
	// The room was closed because the other side hung up; the signal about
	// it was lost.
	auto f = Fixture();
	auto call = Call(f.args());
	Connect(f, call);
	f.media.media->set(Calls::Media::State::Failed);
	f.api.fail(f.api.rejoins, 0, 409, u"call.state"_s);
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(call.endReason(), EndReason::Hangup);
	Settle();
	QCOMPARE(f.api.count(u"hangup"_s), 0);

	auto g = Fixture();
	auto gone = Call(g.args());
	Connect(g, gone, u"ca2"_s);
	g.media.media->set(Calls::Media::State::Failed);
	g.api.fail(g.api.rejoins, 0, 404, u"call.not_found"_s);
	QCOMPARE(gone.state(), State::Ended);
}

void TestCallsCall::rejoinRetriesThenGivesUp() {
	auto f = Fixture();
	auto call = Call(f.args());
	auto gaveUp = 0;
	connect(&call, &Call::reconnectFailed, [&] { ++gaveUp; });
	Connect(f, call);
	auto clock = QElapsedTimer();
	clock.start();
	f.media.media->set(Calls::Media::State::Failed);
	// The server cannot be reached: 20 ms, 40 ms, 80 ms, 80 ms… inside 300 ms.
	auto answered = 0;
	while (call.state() != State::Ended && clock.elapsed() < 10000) {
		if (f.api.rejoins.size() > answered) {
			f.api.fail(f.api.rejoins, answered, 0, u"offline"_s);
			++answered;
		}
		QTest::qWait(2);
	}
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(call.endReason(), EndReason::Failed);
	QVERIFY(!call.quiet());
	QCOMPARE(gaveUp, 1);
	// Not a tight loop, and not before the budget is used up.
	QVERIFY2(answered >= 2 && answered <= 6, qPrintable(QString::number(answered)));
	QVERIFY2(clock.elapsed() >= 280, qPrintable(QString::number(clock.elapsed())));
	// Ended for both sides.
	QCOMPARE(f.api.count(u"hangup"_s), 1);
	const auto requests = f.api.rejoins.size();
	Settle(150);
	QCOMPARE(f.api.rejoins.size(), requests);
}

void TestCallsCall::reconnectBudgetIsShared() {
	// The library keeps "reconnecting" and never gets there: the same 30 s
	// (here 300 ms) apply, nobody waits for ever.
	auto f = Fixture();
	auto call = Call(f.args());
	Connect(f, call);
	f.media.media->set(Calls::Media::State::Reconnecting);
	QTRY_COMPARE_WITH_TIMEOUT(call.state(), State::Ended, 10000);
	QCOMPARE(call.endReason(), EndReason::Failed);
	QCOMPARE(f.api.count(u"hangup"_s), 1);

	// A rejoin that the server answers but the SFU never accepts: connect
	// fails, new credentials, connect fails… against one clock.
	auto g = Fixture();
	auto second = Call(g.args());
	Connect(g, second, u"ca2"_s);
	auto clock = QElapsedTimer();
	clock.start();
	g.media.media->set(Calls::Media::State::Failed);
	auto answered = 0;
	while (second.state() != State::Ended && clock.elapsed() < 10000) {
		if (g.api.rejoins.size() > answered) {
			g.api.answer(g.api.rejoins, answered, Join(u"ca2"_s, u"connected"_s, u"again"_s));
			++answered;
			// The media gives up a little later, as it does (2 s in the app).
			QTimer::singleShot(30, g.media.media, [&] {
				if (g.media.media && second.state() != State::Ended) {
					g.media.media->set(Calls::Media::State::Failed);
				}
			});
		}
		QTest::qWait(2);
	}
	QCOMPARE(second.state(), State::Ended);
	QCOMPARE(second.endReason(), EndReason::Failed);
	QVERIFY(answered >= 2);
}

void TestCallsCall::qualityReports() {
	auto f = Fixture();
	auto call = Call(f.args());
	Connect(f, call);
	QTRY_COMPARE(f.media.statsRequests, 1);
	auto stats = Calls::MediaStats();
	stats.valid = true;
	stats.rttMs = 48.;
	stats.jitterMs = 3.;
	stats.lossPercent = 0.5;
	stats.mos = 4.3;
	stats.relayTcp = true;
	Q_EMIT f.media.media->statsReady(stats);
	QCOMPARE(f.api.reports.size(), 1);
	QCOMPARE(f.api.reports[0].rttMs, 48.);
	QCOMPARE(f.api.reports[0].netType, u"ethernet"_s);
	QVERIFY(f.api.reports[0].relayTcp);
	QVERIFY(f.api.reports[0].durationMs >= 0);
	// Nothing measured yet: nothing reported.
	Q_EMIT f.media.media->statsReady(Calls::MediaStats());
	QCOMPARE(f.api.reports.size(), 1);
	// None after the call.
	call.hangup();
	Q_EMIT f.media.media->statsReady(stats);
	QCOMPARE(f.api.reports.size(), 1);
}

void TestCallsCall::destroyedInCallTellsTheServer() {
	auto f = Fixture();
	{
		auto call = Call(f.args());
		Connect(f, call);
	}
	QCOMPARE(f.api.commands(), (QStringList{ u"connected:ca1"_s, u"hangup:ca1"_s }));
	// One that only rings here is left ringing for my other devices.
	auto g = Fixture();
	{
		auto call = Call(g.args());
		call.ring(u"ca2"_s);
	}
	QCOMPARE(g.api.commands(), QStringList{ u"ringing:ca2"_s });
	// One that rings there is withdrawn.
	auto h = Fixture();
	{
		auto call = Call(h.args());
		call.start();
		h.api.answer(h.api.invites, 0, Join(u"ca3"_s));
	}
	QCOMPARE(h.api.commands(), QStringList{ u"cancel:ca3"_s });
}

void TestCallsCall::exitInCall() {
	auto f = Fixture();
	f.api.holdCommands = true;
	auto call = Call(f.args());
	Connect(f, call);
	auto done = 0;
	call.finishOnExit([&] { ++done; });
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(f.api.commands().last(), u"hangup:ca1"_s);
	QCOMPARE(done, 0); // the app waits for the request to be out
	f.api.finishCommand(int(f.api.commandsSent.size()) - 1);
	QCOMPARE(done, 1);
	QCOMPARE(f.media.leaves, 1);
}

void TestCallsCall::exitWhileRingingHere() {
	auto f = Fixture();
	auto call = Call(f.args());
	call.ring(u"ca1"_s);
	auto done = 0;
	call.finishOnExit([&] { ++done; });
	QCOMPARE(done, 1);
	QCOMPARE(call.state(), State::Ended);
	QCOMPARE(f.api.commands(), QStringList{ u"ringing:ca1"_s });
}

void TestCallsCall::exitBeforeTheCallExists() {
	auto f = Fixture();
	f.api.holdCommands = true;
	auto done = 0;
	{
		auto call = Call(f.args());
		call.start();
		call.finishOnExit([&] { ++done; });
		QCOMPARE(call.state(), State::Ended);
		QCOMPARE(done, 0);
	}
	f.api.answer(f.api.invites, 0, Join(u"ca1"_s));
	QCOMPARE(f.api.commands(), QStringList{ u"cancel:ca1"_s });
	QCOMPARE(done, 0);
	f.api.finishCommand(0);
	QCOMPARE(done, 1);

	// The start was refused: the app does not wait for a cancel.
	auto g = Fixture();
	auto second = Call(g.args());
	auto finished = 0;
	second.start();
	second.finishOnExit([&] { ++finished; });
	g.api.fail(g.api.invites, 0, 409, u"call.busy"_s);
	QCOMPARE(finished, 1);
	QVERIFY(g.api.commandsSent.isEmpty());
}

void TestCallsCall::resumeAfterRestart() {
	auto f = Fixture();
	auto call = Call(f.args());
	auto active = Data::ActiveCall();
	active.call = u"ca1"_s;
	active.state = u"connected"_s;
	active.peer = u"u_sara"_s;
	active.caller = true;
	call.resume(active);
	QCOMPARE(call.state(), State::Reconnecting);
	QVERIFY(call.outgoing());
	QCOMPARE(f.api.rejoins.size(), 1);
	f.api.answer(f.api.rejoins, 0, Join(u"ca1"_s, u"connected"_s, u"fresh"_s));
	QCOMPARE(f.media.joins, QStringList{ u"fresh"_s });
	f.media.media->set(Calls::Media::State::Connected);
	QCOMPARE(call.state(), State::InCall);
	QCOMPARE(f.api.count(u"connected"_s), 1);

	// The other side left while the app was away.
	auto g = Fixture();
	auto gone = Call(g.args());
	gone.resume(active);
	g.api.fail(g.api.rejoins, 0, 409, u"call.state"_s);
	QCOMPARE(gone.state(), State::Ended);
}

// ---- which call

namespace {

struct InstanceFixture {
	FakeApi api;
	FakeMedia::Log media;
	bool microphone = true;
	Calls::MediaProblem problem = Calls::MediaProblem::None;
	QString stored;
	QStringList stores;
	QVector<Calls::Notice> notices;
	std::unique_ptr<Calls::Instance> instance;

	void create(int incomingIntervalMs = 60000) {
		auto args = Calls::Instance::Args();
		args.api = &api;
		args.createMedia = [this]() -> std::unique_ptr<Calls::Media> {
			return std::make_unique<FakeMedia>(&media);
		};
		args.checkMedia = [this] { return problem; };
		args.hasMicrophone = [this] { return microphone; };
		args.networkType = [] { return u"wifi"_s; };
		args.loadStoredCall = [this] { return stored; };
		args.storeCall = [this](const QString &id) {
			stored = id;
			stores.push_back(id);
		};
		args.timings = FastTimings();
		args.timings.ringTimeoutMs = 60000;
		args.incomingCheckIntervalMs = incomingIntervalMs;
		instance = std::make_unique<Calls::Instance>(args);
		QObject::connect(instance.get(), &Calls::Instance::notice, [this](
				Calls::Notice notice) {
			notices.push_back(notice);
		});
	}
};

} // namespace

void TestCallsCall::instanceStartsOneCall() {
	auto f = InstanceFixture();
	f.create();
	QVERIFY(!f.instance->call());
	QVERIFY(f.instance->start(u"u_sara"_s));
	QVERIFY(f.instance->live());
	const auto call = f.instance->call();
	QCOMPARE(call->peerId(), u"u_sara"_s);
	QCOMPARE(f.api.invites.size(), 1);
	// The same person again: that call, not a second one.
	QVERIFY(f.instance->start(u"u_sara"_s));
	QCOMPARE(f.instance->call(), call);
	QCOMPARE(f.api.invites.size(), 1);
	// Somebody else while this one is live: refused with a notice.
	QVERIFY(!f.instance->start(u"u_ali"_s));
	QCOMPARE(f.notices, QVector<Calls::Notice>{ Calls::Notice::AlreadyInCall });
	QCOMPARE(f.api.invites.size(), 1);
	QVERIFY(!f.instance->start(QString()));
}

void TestCallsCall::instanceRingsAndIgnoresSecondCall() {
	auto f = InstanceFixture();
	f.create();
	auto changes = 0;
	connect(f.instance.get(), &Calls::Instance::callChanged, [&] { ++changes; });
	// Whoever shows the call must be told once it is known to be ringing:
	// the last word it hears is about an incoming call.
	auto heard = State::Ended;
	const auto hear = [&] {
		heard = f.instance->call() ? f.instance->call()->state() : State::Ended;
	};
	connect(f.instance.get(), &Calls::Instance::callChanged, hear);
	connect(f.instance.get(), &Calls::Instance::callUpdated, hear);
	f.instance->handleSignal(Signal("call.incoming", u"ca1"_s, QString(), u"u_ali"_s));
	QVERIFY(f.instance->live());
	QCOMPARE(heard, State::Incoming);
	QCOMPARE(f.instance->call()->state(), State::Incoming);
	QCOMPARE(f.instance->call()->peerId(), u"u_ali"_s);
	QCOMPARE(f.instance->call()->id(), u"ca1"_s);
	QCOMPARE(changes, 1);
	// The same ring again (another connection of mine relayed it).
	f.instance->handleSignal(Signal("call.incoming", u"ca1"_s, QString(), u"u_ali"_s));
	QCOMPARE(changes, 1);
	// A second caller: busy, as the server told them.
	f.instance->handleSignal(Signal("call.incoming", u"ca2"_s, QString(), u"u_sara"_s));
	QCOMPARE(f.instance->call()->id(), u"ca1"_s);
	QCOMPARE(changes, 1);
	QCOMPARE(f.api.count(u"reject"_s), 0);

	// Signals reach the call; one that only rang here leaves no trace.
	auto ended = 0;
	connect(f.instance.get(), &Calls::Instance::callEnded, [&] {
		++ended;
		QVERIFY(f.instance->call() != nullptr);
		QVERIFY(f.instance->call()->quiet());
	});
	f.instance->handleSignal(Signal("call.ended", u"ca1"_s, u"cancelled"_s, u"u_ali"_s));
	QCOMPARE(ended, 1);
	QVERIFY(!f.instance->call());
	QCOMPARE(changes, 2);
	// And the next one rings.
	f.instance->handleSignal(Signal("call.incoming", u"ca3"_s, QString(), u"u_sara"_s));
	QCOMPARE(f.instance->call()->id(), u"ca3"_s);
	// Incomplete signals ring nothing.
	f.instance->call()->hangup();
	QVERIFY(!f.instance->call());
	f.instance->handleSignal(Signal("call.incoming", u"ca4"_s, QString(), QString()));
	QVERIFY(!f.instance->call());
}

void TestCallsCall::instanceGlare() {
	// Both press «call» at the same moment. The server lets one call exist;
	// mine is refused as busy while theirs is already ringing here.
	auto f = InstanceFixture();
	f.create();
	QVERIFY(f.instance->start(u"u_sara"_s));
	f.instance->handleSignal(Signal("call.incoming", u"theirs"_s, QString(), u"u_sara"_s));
	QVERIFY(f.instance->call()->outgoing()); // still mine on screen
	f.api.fail(f.api.invites, 0, 409, u"call.busy"_s);
	QVERIFY(f.instance->live());
	QVERIFY(!f.instance->call()->outgoing());
	QCOMPARE(f.instance->call()->state(), State::Incoming);
	QCOMPARE(f.instance->call()->id(), u"theirs"_s);
	QCOMPARE(f.api.count(u"cancel"_s), 0);

	// The other outcome: mine exists, so theirs was the one refused.
	auto g = InstanceFixture();
	g.create();
	QVERIFY(g.instance->start(u"u_sara"_s));
	g.instance->handleSignal(Signal("call.incoming", u"theirs"_s, QString(), u"u_sara"_s));
	g.api.answer(g.api.invites, 0, Join(u"mine"_s));
	QCOMPARE(g.instance->call()->id(), u"mine"_s);
	g.instance->handleSignal(Signal("call.ended", u"mine"_s, u"rejected"_s));
	QVERIFY(g.instance->call()->ended());
	QVERIFY(g.instance->call()->outgoing()); // nothing rings afterwards

	// Theirs was withdrawn before mine failed: nothing rings either.
	auto h = InstanceFixture();
	h.create();
	QVERIFY(h.instance->start(u"u_sara"_s));
	h.instance->handleSignal(Signal("call.incoming", u"theirs"_s, QString(), u"u_sara"_s));
	h.instance->handleSignal(Signal("call.ended", u"theirs"_s, u"cancelled"_s));
	h.api.fail(h.api.invites, 0, 409, u"call.busy"_s);
	QVERIFY(h.instance->call()->ended());
	QCOMPARE(h.instance->call()->endReason(), EndReason::Busy);
}

void TestCallsCall::instanceNoMicrophone() {
	auto f = InstanceFixture();
	f.create();
	f.microphone = false;
	QVERIFY(!f.instance->start(u"u_sara"_s));
	QVERIFY(!f.instance->call());
	QVERIFY(f.api.invites.isEmpty());
	QCOMPARE(f.notices, QVector<Calls::Notice>{ Calls::Notice::NoMicrophone });

	// Answering: said, and it keeps ringing (the phone can still take it).
	f.instance->handleSignal(Signal("call.incoming", u"ca1"_s));
	f.instance->answer();
	QCOMPARE(f.instance->call()->state(), State::Incoming);
	QVERIFY(f.api.accepts.isEmpty());
	QCOMPARE(f.notices.size(), 2);
	// A microphone was plugged in.
	f.microphone = true;
	f.instance->answer();
	QCOMPARE(f.instance->call()->state(), State::Connecting);
	QCOMPARE(f.api.accepts.size(), 1);
}

void TestCallsCall::instanceMediaMissing() {
	auto f = InstanceFixture();
	f.create();
	f.problem = Calls::MediaProblem::LibraryMissing;
	QVERIFY(!f.instance->start(u"u_sara"_s));
	QVERIFY(f.api.invites.isEmpty());
	QCOMPARE(f.notices, QVector<Calls::Notice>{ Calls::Notice::MediaUnavailable });
	f.instance->handleSignal(Signal("call.incoming", u"ca1"_s));
	QCOMPARE(f.instance->call()->state(), State::Incoming); // it rings: decline works
	f.instance->answer();
	QVERIFY(f.api.accepts.isEmpty());
	QCOMPARE(f.notices.size(), 2);
	f.instance->call()->hangup();
	QCOMPARE(f.api.count(u"reject"_s), 1);
}

void TestCallsCall::instanceIncomingCheckOncePerMinute() {
	// "Does something ring for me?" is asked when the session starts, when
	// the connection comes up, and once per interval while it is down -
	// never more often than once per interval (here 120 ms).
	auto f = InstanceFixture();
	f.api.online = false;
	f.create(120);
	QCOMPARE(f.api.incomingRequests, 0);
	f.instance->sessionReady();
	QCOMPARE(f.api.incomingRequests, 1);
	f.api.incomingDone(std::nullopt);
	// The connection flaps.
	for (auto i = 0; i != 5; ++i) {
		f.api.online = true;
		f.instance->signalsConnectedChanged(true);
		f.api.online = false;
		f.instance->signalsConnectedChanged(false);
	}
	QCOMPARE(f.api.incomingRequests, 1);
	// Down: once per interval.
	QTRY_COMPARE_WITH_TIMEOUT(f.api.incomingRequests, 2, 5000);
	f.api.incomingDone(std::nullopt);
	QTRY_COMPARE_WITH_TIMEOUT(f.api.incomingRequests, 3, 5000);
	// Something rings.
	auto info = Info(u"ca1"_s, u"ringing"_s);
	info.peer = u"u_ali"_s;
	f.api.incomingDone(info);
	QVERIFY(f.instance->live());
	QCOMPARE(f.instance->call()->state(), State::Incoming);
	QCOMPARE(f.instance->call()->peerId(), u"u_ali"_s);
	// Not asked while a call is there.
	const auto asked = f.api.incomingRequests;
	Settle(300);
	QCOMPARE(f.api.incomingRequests, asked);

	// Up: the connection's signals do the work, nothing is asked on a timer.
	auto g = InstanceFixture();
	g.create(60);
	g.instance->sessionReady();
	g.api.incomingDone(std::nullopt);
	Settle(250);
	QCOMPARE(g.api.incomingRequests, 1);
	// A real reconnect after more than the interval asks once.
	g.api.online = false;
	g.instance->signalsConnectedChanged(false);
	g.api.online = true;
	g.instance->signalsConnectedChanged(true);
	QCOMPARE(g.api.incomingRequests, 2);
}

void TestCallsCall::instanceRestartRecovery_data() {
	QTest::addColumn<QString>("stored");
	QTest::addColumn<QString>("activeCall"); // "" = none
	QTest::addColumn<QString>("activeState");
	QTest::addColumn<bool>("caller");
	QTest::addColumn<QString>("expect"); // resume | cancel | forget | nothing

	QTest::newRow("no call before the restart") << QString() << u"ca1"_s << u"connected"_s << true << u"nothing"_s;
	QTest::newRow("in a call, still there") << u"ca1"_s << u"ca1"_s << u"connected"_s << true << u"resume"_s;
	QTest::newRow("answered, audio not up yet") << u"ca1"_s << u"ca1"_s << u"accepted"_s << false << u"resume"_s;
	QTest::newRow("it was ringing there") << u"ca1"_s << u"ca1"_s << u"ringing"_s << true << u"cancel"_s;
	QTest::newRow("it ended meanwhile") << u"ca1"_s << QString() << QString() << false << u"forget"_s;
	QTest::newRow("another call now (on the phone)") << u"ca1"_s << u"ca2"_s << u"connected"_s << true << u"forget"_s;
}

void TestCallsCall::instanceRestartRecovery() {
	QFETCH(QString, stored);
	QFETCH(QString, activeCall);
	QFETCH(QString, activeState);
	QFETCH(bool, caller);
	QFETCH(QString, expect);
	auto f = InstanceFixture();
	f.stored = stored;
	f.create();
	f.instance->sessionReady();
	if (expect == u"nothing") {
		// Whatever call the user is in elsewhere is not this device's.
		QCOMPARE(f.api.activeRequests, 0);
		QCOMPARE(f.api.incomingRequests, 1);
		QVERIFY(!f.instance->call());
		return;
	}
	QCOMPARE(f.api.activeRequests, 1);
	QCOMPARE(f.api.incomingRequests, 0);
	if (activeCall.isEmpty()) {
		f.api.activeDone(std::nullopt);
	} else {
		auto active = Data::ActiveCall();
		active.call = activeCall;
		active.state = activeState;
		active.peer = u"u_sara"_s;
		active.caller = caller;
		f.api.activeDone(active);
	}
	if (expect == u"resume") {
		QVERIFY(f.instance->live());
		QCOMPARE(f.instance->call()->state(), State::Reconnecting);
		QCOMPARE(f.instance->call()->id(), u"ca1"_s);
		QCOMPARE(f.instance->call()->outgoing(), caller);
		QCOMPARE(f.api.rejoins.size(), 1);
		QCOMPARE(f.stored, u"ca1"_s);
	} else if (expect == u"cancel") {
		QVERIFY(!f.instance->call());
		QCOMPARE(f.api.commands(), QStringList{ u"cancel:ca1"_s });
		QVERIFY(f.stored.isEmpty());
	} else {
		QVERIFY(!f.instance->call());
		QVERIFY(f.api.commandsSent.isEmpty());
		QVERIFY(f.stored.isEmpty());
		QCOMPARE(f.api.incomingRequests, 1);
	}
}

void TestCallsCall::instanceEndedCallStaysUntilDismissed() {
	auto f = InstanceFixture();
	f.create();
	QVERIFY(f.instance->start(u"u_sara"_s));
	f.api.answer(f.api.invites, 0, Join(u"ca1"_s));
	f.instance->handleSignal(Signal("call.ended", u"ca1"_s, u"rejected"_s));
	// «رد شد» is on screen: the call object is still there, but not live.
	QVERIFY(f.instance->call() != nullptr);
	QVERIFY(!f.instance->live());
	QCOMPARE(f.instance->call()->endReason(), EndReason::Declined);
	f.instance->dismissEnded();
	QVERIFY(!f.instance->call());
	f.instance->dismissEnded();

	// «تماس دوباره» / an incoming call replace the ended one at once.
	QVERIFY(f.instance->start(u"u_sara"_s));
	f.api.fail(f.api.invites, 1, 409, u"call.busy"_s);
	QVERIFY(f.instance->call()->ended());
	QVERIFY(f.instance->start(u"u_sara"_s));
	QVERIFY(f.instance->live());
	QCOMPARE(f.api.invites.size(), 3);
	f.instance->call()->hangup();
	QVERIFY(!f.instance->call());
	QVERIFY(f.instance->start(u"u_sara"_s));
	f.api.fail(f.api.invites, 3, 403, u"call.not_allowed"_s);
	f.instance->handleSignal(Signal("call.incoming", u"ca9"_s, QString(), u"u_ali"_s));
	QVERIFY(f.instance->live());
	QCOMPARE(f.instance->call()->id(), u"ca9"_s);
}

void TestCallsCall::instanceStoresTheCall() {
	// Outgoing: remembered from the moment it exists, forgotten when over.
	auto f = InstanceFixture();
	f.create();
	QVERIFY(f.instance->start(u"u_sara"_s));
	QVERIFY(f.stores.isEmpty());
	f.api.answer(f.api.invites, 0, Join(u"ca1"_s));
	QCOMPARE(f.stored, u"ca1"_s);
	f.instance->call()->hangup();
	QVERIFY(f.stored.isEmpty());

	// Incoming: only once it was answered here (a call that just rings
	// may be answered on the phone).
	auto g = InstanceFixture();
	g.create();
	g.instance->handleSignal(Signal("call.incoming", u"ca2"_s));
	QVERIFY(g.stored.isEmpty());
	g.instance->answer();
	QCOMPARE(g.stored, u"ca2"_s);
	g.api.answer(g.api.accepts, 0, Join(u"ca2"_s, u"accepted"_s));
	g.media.media->set(Calls::Media::State::Connected);
	QCOMPARE(g.instance->call()->state(), State::InCall);
	// Quitting ends the call on purpose: nothing to come back to.
	auto done = 0;
	g.instance->finishOnExit([&] { ++done; });
	QVERIFY(g.stored.isEmpty());
	QCOMPARE(g.api.commands().last(), u"hangup:ca2"_s);
	QTRY_COMPARE(done, 1);

	auto h = InstanceFixture();
	h.create();
	auto idle = 0;
	h.instance->finishOnExit([&] { ++idle; });
	QCOMPARE(idle, 1);
}

QTEST_GUILESS_MAIN(TestCallsCall)

#include "test_calls_call.moc"
