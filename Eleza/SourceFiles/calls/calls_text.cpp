#include "calls/calls_text.h"

#include "data/data_session.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/error_text.h"

namespace Calls {

QString ClockText(qint64 seconds) {
	seconds = std::max(seconds, qint64(0));
	const auto hours = seconds / 3600;
	const auto minutes = (seconds % 3600) / 60;
	const auto rest = seconds % 60;
	const auto two = [](qint64 value) {
		return QStringLiteral("%1").arg(value, 2, 10, QLatin1Char('0'));
	};
	return Lang::Num(hours
		? (QString::number(hours) + u':' + two(minutes) + u':' + two(rest))
		: (two(minutes) + u':' + two(rest)));
}

QString StateText(const Call &call, qint64 nowMs) {
	switch (call.state()) {
	case State::Calling: return tr::lng_calls_state_calling(tr::now);
	case State::Ringing: return tr::lng_calls_state_ringing(tr::now);
	case State::Incoming: return tr::lng_calls_state_incoming(tr::now);
	case State::Connecting: return tr::lng_calls_state_connecting(tr::now);
	case State::Reconnecting:
		return tr::lng_calls_state_reconnecting(tr::now);
	case State::InCall:
		return ClockText(call.connectedAt()
			? ((nowMs - call.connectedAt()) / 1000)
			: 0);
	case State::Ended: return EndedText(call);
	}
	return QString();
}

QString EndedText(const Call &call) {
	switch (call.endReason()) {
	case EndReason::Declined: return tr::lng_calls_ended_declined(tr::now);
	case EndReason::NoAnswer: return tr::lng_calls_ended_no_answer(tr::now);
	case EndReason::Busy: return tr::lng_calls_ended_busy(tr::now);
	case EndReason::NotAllowed:
		return tr::lng_calls_ended_not_allowed(tr::now);
	case EndReason::Failed: {
		// What the server said, when it said something a person can read.
		const auto &error = call.startError();
		const auto known = error
			? Support::ErrorPhrase(error->code)
			: QString();
		return known.isEmpty() ? tr::lng_calls_ended_failed(tr::now) : known;
	}
	case EndReason::Lost: return tr::lng_calls_ended_lost(tr::now);
	case EndReason::AnsweredElsewhere:
		return tr::lng_calls_answered_elsewhere(tr::now);
	case EndReason::Hangup:
	case EndReason::DeclinedByMe:
	case EndReason::Missed:
	case EndReason::None:
		break;
	}
	const auto duration = call.duration();
	return (duration > 0)
		? tr::lng_calls_ended_duration(
			tr::now,
			lt_duration,
			ClockText(duration))
		: tr::lng_calls_ended(tr::now);
}

bool StateIsWarning(const Call &call) {
	if (call.state() == State::Reconnecting) {
		return true;
	} else if (call.state() != State::Ended) {
		return false;
	}
	switch (call.endReason()) {
	case EndReason::Busy:
	case EndReason::NotAllowed:
	case EndReason::Failed:
	case EndReason::Lost:
		return true;
	default: break;
	}
	return false;
}

bool CanCallAgain(const Call &call) {
	if (!call.ended() || call.quiet()) {
		return false;
	}
	switch (call.endReason()) {
	case EndReason::NotAllowed:
		return false; // the answer would be the same
	case EndReason::Hangup:
	case EndReason::Declined:
	case EndReason::NoAnswer:
	case EndReason::Busy:
	case EndReason::Failed:
	case EndReason::Lost:
		return true;
	default: break;
	}
	return false;
}

QString NoticeText(Notice notice) {
	switch (notice) {
	case Notice::NoMicrophone: return tr::lng_calls_no_microphone(tr::now);
	case Notice::MediaUnavailable: return tr::lng_calls_unavailable(tr::now);
	case Notice::AlreadyInCall: return tr::lng_calls_already_in_call(tr::now);
	case Notice::AnsweredElsewhere:
		return tr::lng_calls_answered_elsewhere(tr::now);
	case Notice::ReconnectFailed:
		return tr::lng_calls_reconnect_failed(tr::now);
	case Notice::MicrophoneFailed:
		return tr::lng_calls_microphone_failed(tr::now);
	case Notice::InputDeviceChanged:
		return tr::lng_calls_input_changed(tr::now);
	case Notice::OutputDeviceChanged:
		return tr::lng_calls_output_changed(tr::now);
	}
	return QString();
}

QString PeerName(Main::Session *session, const QString &userId) {
	if (!session || userId.isEmpty()) {
		return QString();
	}
	const auto user = session->data().users().get(userId);
	if (!user.displayName.isEmpty()) {
		return user.displayName;
	} else if (!user.handle.isEmpty()) {
		return Lang::Handle(user.handle);
	}
	return QString();
}

} // namespace Calls
