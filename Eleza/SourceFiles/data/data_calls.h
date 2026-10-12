// Voice calls as the server sees them: the commands of the `/v1/calls` routes
// and the live `call.*` signals.
//
// A call is created by the caller (POST /v1/calls), rings for the callee
// (signal call.incoming; GET /v1/calls/incoming for a device that was offline
// then) and moves through ringing -> accepted -> connected -> ended on the
// server; commands are plain HTTPS, what the other side did arrives as signals
// on the realtime connection:
//
// (Every signal of a call that was started as a video call also carries
// `video: true`; so do the answers below.)
//
//   call.incoming            a call rings for me            { call, from }
//   call.ringing             my call rings on their device  { call }
//   call.accepted            they answered                  { call }
//   call.answered_elsewhere  another device of mine answered{ call, device }
//   call.ended               over                           { call, reason }
//
// A participant polls GET /v1/calls/{id} while its call is up: that is also
// its heartbeat (a connected call nobody polls ends with the reason "lost"),
// and the way the state arrives when the connection is down.
//
// The audio itself never comes here: an answer that lets this device into the
// call carries the join payload for the media layer (calls/calls_media.h).
//
// Data::CallsApi is the interface the call logic uses (calls/calls_call.h is
// tested against a fake); Data::Calls implements it over Api::Client.
#pragma once

#include "api/api_error.h"
#include "base/weak_guard.h"
#include "data/data_social_types.h"
#include "data/data_types.h"

#include <QtCore/QJsonObject>
#include <QtCore/QObject>

#include <optional>

namespace Rt {
struct Frame;
} // namespace Rt

namespace Data {

class Session;

// The answer of POST /v1/calls, …/accept and …/rejoin.
struct CallJoin {
	QString call;
	QString state;   // ringing | accepted | connected
	QString peer;    // user id
	// The call was started as a video call (the intent; who shows a camera
	// is a matter of the tracks in the room).
	bool video = false;
	// The whole answer: sfu_url, token, token_expires_at, turn, relay_only.
	QJsonObject media;

	[[nodiscard]] bool valid() const {
		return !call.isEmpty();
	}
	[[nodiscard]] static CallJoin FromJson(const QJsonObject &json);
};

// GET /v1/calls/{id} and GET /v1/calls/incoming.
struct CallInfo {
	QString call;
	QString state;      // ringing | accepted | connected | ended
	QString endReason;  // rejected | cancelled | hangup | timeout | network | lost | max_duration | blocked
	QString peer;
	bool outgoing = false;
	qint64 createdAt = 0; // unix seconds
	qint64 durationS = -1; // of an ended call that was connected; -1: not told
	bool delivered = false; // an outgoing ringing call: their device rings
	bool video = false;     // started as a video call

	[[nodiscard]] bool valid() const {
		return !call.isEmpty();
	}
	[[nodiscard]] static CallInfo FromJson(const QJsonObject &json);
};

// GET /v1/calls/active: the call this user is in on any of their devices.
struct ActiveCall {
	QString call;
	QString state; // ringing | accepted | connected
	QString peer;
	bool caller = false;
	bool video = false;
	qint64 createdAt = 0;

	[[nodiscard]] static ActiveCall FromJson(const QJsonObject &json);
};

struct CallSignal {
	enum class Type {
		Unknown,
		Incoming,
		Ringing,
		Accepted,
		AnsweredElsewhere,
		Ended,
	};
	Type type = Type::Unknown;
	QString call;
	QString state;
	QString reason;
	QString from;   // the other user
	QString device; // answered_elsewhere: the device that answered
	bool video = false; // on every signal of a call started as a video call

	// From a frame of Rt::Frame::Type::Call; Unknown for anything else.
	[[nodiscard]] static CallSignal FromFrame(const Rt::Frame &frame);
	[[nodiscard]] static CallSignal FromJson(
		const QString &type,
		const QJsonObject &json);
};

// POST /v1/calls/{id}/qos: numbers about the connection, no content.
struct CallQos {
	double rttMs = 0.;
	double jitterMs = 0.;
	double lossPercent = 0.;
	double mos = 0.;
	QString netType = u"unknown"_s; // wifi | cellular | ethernet | unknown
	bool relayTcp = false;
	qint64 durationMs = 0;

	[[nodiscard]] QJsonObject toJson() const;
};

// How a call record of the chat history reads (the server stores the outcome
// once; each side words it by direction).
enum class CallRecordKind {
	Outgoing,          // answered, I called
	Incoming,          // answered, they called
	OutgoingNoAnswer,  // I called, nobody answered (or I gave up)
	Missed,            // they called, I did not answer
	OutgoingDeclined,  // I called, they declined
	IncomingDeclined,  // they called, I declined
};
[[nodiscard]] CallRecordKind ClassifyCallRecord(const CallRecord &record, bool mine);
// A red entry: something the user may have missed.
[[nodiscard]] bool CallRecordIsMissed(CallRecordKind kind);

// Whether the chat's header offers a call. The server allows calls only
// inside an active direct chat of two real accounts.
enum class CallAvailability {
	Hidden,     // not a chat that can ever have calls (conversation, bot, saved, deleted)
	Disabled,   // a direct chat the server would refuse right now (request, blocked)
	Available,
};
[[nodiscard]] CallAvailability CallAvailabilityFor(const Chat &chat);

class CallsApi {
public:
	using Done = Fn<void()>;
	using JoinDone = Fn<void(const CallJoin &)>;

	virtual ~CallsApi() = default;

	// `video`: the call starts as a video call. Only where the server lists
	// `feature.video_calls` (an older one refuses the unknown field).
	virtual void invite(
		const QString &userId,
		bool video,
		base::Guard guard,
		JoinDone done,
		FailCallback fail) = 0;
	virtual void accept(
		const QString &call,
		base::Guard guard,
		JoinDone done,
		FailCallback fail) = 0;
	virtual void rejoin(
		const QString &call,
		base::Guard guard,
		JoinDone done,
		FailCallback fail) = 0;
	virtual void get(
		const QString &call,
		base::Guard guard,
		Fn<void(const CallInfo &)> done,
		FailCallback fail) = 0;
	// nullopt: nothing rings for me.
	virtual void incoming(
		base::Guard guard,
		Fn<void(const std::optional<CallInfo> &)> done,
		FailCallback fail) = 0;
	// nullopt: I am in no call.
	virtual void active(
		base::Guard guard,
		Fn<void(const std::optional<ActiveCall> &)> done,
		FailCallback fail) = 0;
	// "ringing" | "reject" | "cancel" | "hangup" | "connected".
	virtual void command(
		const QString &call,
		const QString &verb,
		base::Guard guard = base::Guard(),
		Done done = nullptr,
		FailCallback fail = nullptr) = 0;
	virtual void qos(const QString &call, const CallQos &qos) = 0;
	// The realtime connection is up: signals arrive.
	[[nodiscard]] virtual bool signalsConnected() const = 0;
};

class Calls final : public QObject, public CallsApi {
	Q_OBJECT

public:
	explicit Calls(Session *session);

	void invite(
		const QString &userId,
		bool video,
		base::Guard guard,
		JoinDone done,
		FailCallback fail) override;
	void accept(
		const QString &call,
		base::Guard guard,
		JoinDone done,
		FailCallback fail) override;
	void rejoin(
		const QString &call,
		base::Guard guard,
		JoinDone done,
		FailCallback fail) override;
	void get(
		const QString &call,
		base::Guard guard,
		Fn<void(const CallInfo &)> done,
		FailCallback fail) override;
	void incoming(
		base::Guard guard,
		Fn<void(const std::optional<CallInfo> &)> done,
		FailCallback fail) override;
	void active(
		base::Guard guard,
		Fn<void(const std::optional<ActiveCall> &)> done,
		FailCallback fail) override;
	void command(
		const QString &call,
		const QString &verb,
		base::Guard guard = base::Guard(),
		Done done = nullptr,
		FailCallback fail = nullptr) override;
	void qos(const QString &call, const CallQos &qos) override;
	[[nodiscard]] bool signalsConnected() const override;

Q_SIGNALS:
	void signalReceived(const Data::CallSignal &signal);
	// The realtime connection came up or went down.
	void signalsConnectedChanged(bool connected);

private:
	void post(
		const QString &path,
		const QJsonObject &body,
		base::Guard guard,
		Fn<void(const QJsonObject &)> done,
		FailCallback fail);

	Session *_session = nullptr;

};

} // namespace Data

Q_DECLARE_METATYPE(Data::CallSignal)
