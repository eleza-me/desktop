#include "data/data_calls.h"

#include "api/api_client.h"
#include "data/data_json.h"
#include "data/data_session.h"
#include "data/data_transport.h"
#include "rt/rt_client.h"

#include <QtCore/QUrl>

namespace Data {
namespace {

using namespace Json;

[[nodiscard]] QString CallPath(const QString &call, const QString &verb = QString()) {
	auto result = u"/v1/calls/"_s
		+ QString::fromLatin1(QUrl::toPercentEncoding(call));
	if (!verb.isEmpty()) {
		result += u'/' + verb;
	}
	return result;
}

} // namespace

CallJoin CallJoin::FromJson(const QJsonObject &json) {
	auto result = CallJoin();
	result.call = String(json, "call"_L1);
	result.state = String(json, "state"_L1);
	result.peer = String(json, "peer"_L1);
	result.video = Bool(json, "video"_L1);
	result.media = json;
	return result;
}

CallInfo CallInfo::FromJson(const QJsonObject &json) {
	auto result = CallInfo();
	result.call = String(json, "call"_L1);
	result.state = String(json, "state"_L1);
	result.endReason = String(json, "end_reason"_L1);
	result.peer = String(json, "peer"_L1);
	result.outgoing = Bool(json, "outgoing"_L1);
	result.createdAt = Int64(json, "created_at"_L1);
	result.durationS = Has(json, "duration_s"_L1)
		? Int64(json, "duration_s"_L1)
		: -1;
	result.delivered = Bool(json, "delivered"_L1);
	result.video = Bool(json, "video"_L1);
	return result;
}

ActiveCall ActiveCall::FromJson(const QJsonObject &json) {
	auto result = ActiveCall();
	result.call = String(json, "call"_L1);
	result.state = String(json, "state"_L1);
	result.peer = String(json, "peer"_L1);
	result.caller = (String(json, "role"_L1) == u"caller");
	result.video = Bool(json, "video"_L1);
	result.createdAt = Int64(json, "created_at"_L1);
	return result;
}

CallSignal CallSignal::FromJson(const QString &type, const QJsonObject &json) {
	auto result = CallSignal();
	result.type = (type == u"call.incoming")
		? Type::Incoming
		: (type == u"call.ringing")
		? Type::Ringing
		: (type == u"call.accepted")
		? Type::Accepted
		: (type == u"call.answered_elsewhere")
		? Type::AnsweredElsewhere
		: (type == u"call.ended")
		? Type::Ended
		: Type::Unknown;
	result.call = String(json, "call"_L1);
	result.state = String(json, "state"_L1);
	result.reason = String(json, "reason"_L1);
	result.from = String(json, "from"_L1);
	result.device = String(json, "device"_L1);
	result.video = Bool(json, "video"_L1);
	if (result.call.isEmpty()) {
		result.type = Type::Unknown;
	}
	return result;
}

CallSignal CallSignal::FromFrame(const Rt::Frame &frame) {
	if (frame.type != Rt::Frame::Type::Call) {
		return CallSignal();
	}
	// The body's own "t" names the signal as well; the frame's kind wins.
	return FromJson(frame.kind, frame.data);
}

QJsonObject CallQos::toJson() const {
	return {
		{ u"rtt_ms"_s, rttMs },
		{ u"jitter_ms"_s, jitterMs },
		{ u"loss_pct"_s, lossPercent },
		{ u"mos"_s, mos },
		{ u"net_type"_s, netType },
		{ u"relay_tcp"_s, relayTcp },
		{ u"duration_ms"_s, double(durationMs) },
	};
}

CallRecordKind ClassifyCallRecord(const CallRecord &record, bool mine) {
	const auto answered = (record.durationS > 0)
		|| (record.outcome == u"completed")
		|| (record.outcome == u"answered");
	const auto declined = (record.outcome == u"declined")
		|| (record.outcome == u"rejected");
	if (answered) {
		return mine ? CallRecordKind::Outgoing : CallRecordKind::Incoming;
	} else if (declined) {
		return mine
			? CallRecordKind::OutgoingDeclined
			: CallRecordKind::IncomingDeclined;
	}
	return mine ? CallRecordKind::OutgoingNoAnswer : CallRecordKind::Missed;
}

bool CallRecordIsMissed(CallRecordKind kind) {
	return (kind == CallRecordKind::Missed)
		|| (kind == CallRecordKind::IncomingDeclined);
}

CallAvailability CallAvailabilityFor(const Chat &chat) {
	if (chat.isConversation
		|| chat.anonymous
		|| chat.saved
		|| chat.isBot
		|| chat.peerDeleted
		|| chat.peerUserId.isEmpty()) {
		return CallAvailability::Hidden;
	}
	// A message request is not a chat of two yet; a chat nobody can write
	// into is blocked from one side.
	if (chat.isRequest()
		|| !chat.request.isEmpty()
		|| !chat.canSend
		|| (!chat.state.isEmpty() && chat.state != u"active")) {
		return CallAvailability::Disabled;
	}
	return CallAvailability::Available;
}

Calls::Calls(Session *session)
: QObject(session)
, _session(session) {
	qRegisterMetaType<Data::CallSignal>();
	connect(
		&session->transport(),
		&Transport::frameReceived,
		this,
		[this](const Rt::Frame &frame) {
			if (frame.type != Rt::Frame::Type::Call) {
				return;
			}
			const auto signal = CallSignal::FromFrame(frame);
			if (signal.type != CallSignal::Type::Unknown) {
				Q_EMIT signalReceived(signal);
			}
		});
	connect(
		&session->transport(),
		&Transport::connectedChanged,
		this,
		&Calls::signalsConnectedChanged);
}

bool Calls::signalsConnected() const {
	return _session->transport().connected();
}

void Calls::post(
		const QString &path,
		const QJsonObject &body,
		base::Guard guard,
		Fn<void(const QJsonObject &)> done,
		FailCallback fail) {
	_session->api().request(Api::Post, path)
		.body(body)
		.guard(this)
		.timeout(15000)
		.done([=](const QJsonObject &result) {
			if (done && guard.alive()) {
				done(result);
			}
		})
		.fail([=](const Api::Error &error) {
			if (fail && guard.alive()) {
				fail(error);
			}
		})
		.send();
}

void Calls::invite(
		const QString &userId,
		bool video,
		base::Guard guard,
		JoinDone done,
		FailCallback fail) {
	auto body = QJsonObject{ { u"user_id"_s, userId } };
	if (video) {
		// Never `false`: a server without video calls refuses the field.
		body.insert(u"video"_s, true);
	}
	post(u"/v1/calls"_s, body, guard, [=](
			const QJsonObject &result) {
		done(CallJoin::FromJson(result));
	}, fail);
}

void Calls::accept(
		const QString &call,
		base::Guard guard,
		JoinDone done,
		FailCallback fail) {
	post(CallPath(call, u"accept"_s), {}, guard, [=](
			const QJsonObject &result) {
		done(CallJoin::FromJson(result));
	}, fail);
}

void Calls::rejoin(
		const QString &call,
		base::Guard guard,
		JoinDone done,
		FailCallback fail) {
	post(CallPath(call, u"rejoin"_s), {}, guard, [=](
			const QJsonObject &result) {
		done(CallJoin::FromJson(result));
	}, fail);
}

void Calls::get(
		const QString &call,
		base::Guard guard,
		Fn<void(const CallInfo &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, CallPath(call))
		.guard(this)
		.timeout(15000)
		.done([=](const QJsonObject &result) {
			if (done && guard.alive()) {
				done(CallInfo::FromJson(result));
			}
		})
		.fail([=](const Api::Error &error) {
			if (fail && guard.alive()) {
				fail(error);
			}
		})
		.send();
}

void Calls::incoming(
		base::Guard guard,
		Fn<void(const std::optional<CallInfo> &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/calls/incoming"_s)
		.guard(this)
		.timeout(15000)
		.done([=](const QJsonObject &result) {
			if (!done || !guard.alive()) {
				return;
			}
			// {} when nothing rings.
			const auto info = CallInfo::FromJson(result);
			done((info.valid() && !info.outgoing && info.state == u"ringing")
				? std::make_optional(info)
				: std::nullopt);
		})
		.fail([=](const Api::Error &error) {
			if (fail && guard.alive()) {
				fail(error);
			}
		})
		.send();
}

void Calls::active(
		base::Guard guard,
		Fn<void(const std::optional<ActiveCall> &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/calls/active"_s)
		.guard(this)
		.timeout(15000)
		.done([=](const QJsonObject &result) {
			if (!done || !guard.alive()) {
				return;
			}
			const auto call = ActiveCall::FromJson(result);
			done(call.call.isEmpty() ? std::nullopt : std::make_optional(call));
		})
		.fail([=](const Api::Error &error) {
			if (!guard.alive()) {
				return;
			}
			if (error.notFound()) {
				// 404 call.not_found is the answer "no call".
				if (done) {
					done(std::nullopt);
				}
			} else if (fail) {
				fail(error);
			}
		})
		.send();
}

void Calls::command(
		const QString &call,
		const QString &verb,
		base::Guard guard,
		Done done,
		FailCallback fail) {
	post(CallPath(call, verb), {}, guard, [=](const QJsonObject &) {
		if (done) {
			done();
		}
	}, fail);
}

void Calls::qos(const QString &call, const CallQos &qos) {
	// Best effort: a report that does not arrive is not worth a word.
	post(CallPath(call, u"qos"_s), qos.toJson(), this, nullptr, nullptr);
}

} // namespace Data
