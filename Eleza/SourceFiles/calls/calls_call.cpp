#include "calls/calls_call.h"

#include <QtCore/QDateTime>
#include <QtCore/QPointer>

#include <algorithm>

namespace Calls {

EndReason EndReasonFromServer(const QString &reason) {
	if (reason == u"rejected") {
		return EndReason::Declined;
	} else if (reason == u"timeout"
		|| reason == u"no_answer"
		|| reason == u"missed") {
		return EndReason::NoAnswer;
	} else if (reason == u"busy") {
		return EndReason::Busy;
	} else if (reason == u"failed") {
		return EndReason::Failed;
	} else if (reason == u"lost" || reason == u"network") {
		// "network": answered, but the audio of one side never arrived.
		return EndReason::Lost;
	}
	// hangup, cancelled, max_duration, blocked and whatever comes later
	// read as a plain end.
	return EndReason::Hangup;
}

EndReason EndReasonFromStartError(const Api::Error &error) {
	if (error.code == u"call.busy") {
		return EndReason::Busy;
	} else if (error.code == u"call.not_allowed") {
		return EndReason::NotAllowed;
	}
	return EndReason::Failed;
}

Call::Call(Args args, QObject *parent)
: QObject(parent)
, _args(std::move(args))
, _pollTimer([this] { poll(); })
, _ringTimer([this] {
	// Nobody rings forever: the server ends a ringing call by itself; this
	// is for a device that did not hear about it.
	if (_state == State::Incoming) {
		end(EndReason::Missed, true);
	}
})
, _rejoinTimer([this] { rejoin(); })
, _reconnectWatchdog([this] {
	if (_reconnectClock.isValid()) {
		failCall();
	}
})
, _qosTimer([this] {
	if (_state == State::InCall && _media) {
		_media->requestStats();
	}
	scheduleQos(_args.timings.qosEachMs);
})
, _pendingStart(std::make_shared<PendingStart>()) {
	Q_ASSERT(_args.api != nullptr);
	_signalsConnected = _args.api->signalsConnected();
}

Call::~Call() {
	// Destroyed while live (the session ended): the server is told, the
	// answer is nobody's business any more.
	if (_started && _outgoing && _id.isEmpty()) {
		_pendingStart->abandoned = true;
	}
	if (shouldTellOnExit()) {
		tellServer(_state);
	}
	if (_media) {
		_media->leave();
	}
}

qint64 Call::duration() const {
	if (_endedDuration >= 0) {
		return _endedDuration;
	}
	return -1;
}

void Call::setState(State state) {
	if (_state == state) {
		return;
	}
	_state = state;
	if (state == State::InCall && !_connectedAt) {
		_connectedAt = QDateTime::currentMSecsSinceEpoch();
	}
	if (state == State::InCall) {
		if (!_qosTimer.isActive()) {
			scheduleQos(_args.timings.qosFirstMs);
		}
	} else {
		_qosTimer.cancel();
	}
	if (state == State::Ended) {
		// Nothing may retry, mute or fail a call that is over.
		_pollTimer.cancel();
		_ringTimer.cancel();
		_rejoinTimer.cancel();
		_reconnectWatchdog.cancel();
		_reconnectClock.invalidate();
		_earlySignals.clear();
		if (_media) {
			_media->leave();
		}
		_cameraWanted = false;
		_remoteVideo = false;
	}
	Q_EMIT stateChanged(state);
}

void Call::end(EndReason reason, bool quiet) {
	if (_state == State::Ended) {
		return;
	}
	_endReason = reason;
	_quiet = quiet;
	if (_endedDuration < 0 && _connectedAt > 0) {
		_endedDuration = std::max(
			(QDateTime::currentMSecsSinceEpoch() - _connectedAt) / 1000,
			qint64(0));
	}
	setState(State::Ended);
}

// ---- starting

void Call::start() {
	if (_started) {
		return;
	}
	_started = true;
	_outgoing = true;
	_state = State::Calling;
	_video = _args.video;
	// A video call starts with my camera (it shows me my own picture while
	// it rings there).
	_cameraWanted = _video;
	sendStart();
}

void Call::setVideo(bool video) {
	if (_video != video) {
		_video = video;
		Q_EMIT videoChanged(video);
	}
}

void Call::sendStart() {
	// The answer is handled also when this object is gone by then: a call
	// that was created after End was pressed has to be cancelled.
	const auto api = _args.api;
	const auto weak = QPointer<Call>(this);
	const auto pending = _pendingStart;
	api->invite(_args.peerId, _video, base::Guard(), [=](const Data::CallJoin &join) {
		pending->answered = true;
		if (pending->abandoned) {
			// Without the cancel their device would ring until the
			// server's own timeout.
			const auto after = pending->afterCancel;
			if (join.valid()) {
				api->command(join.call, u"cancel"_s, base::Guard(), after, [=](
						const Api::Error &) {
					if (after) {
						after();
					}
				});
			} else if (after) {
				after();
			}
			return;
		}
		if (weak) {
			weak->started(join);
		}
	}, [=](const Api::Error &error) {
		pending->answered = true;
		if (pending->abandoned) {
			if (const auto after = pending->afterCancel) {
				after();
			}
			return;
		}
		if (weak) {
			weak->startFailed(error);
		}
	});
}

void Call::started(const Data::CallJoin &join) {
	if (_state == State::Ended) {
		return;
	}
	if (!join.valid()) {
		_told = true;
		_startError = Api::Error::Local(0, u"decoding"_s, u"decoding"_s);
		end(EndReason::Failed);
		return;
	}
	_id = join.call;
	if (join.video) {
		setVideo(true);
	}
	Q_EMIT idAssigned(_id);
	joinMedia(join);
	if (_state == State::Ended) {
		return;
	}
	applyServerState(
		join.state.isEmpty() ? u"ringing"_s : join.state,
		QString(),
		-1);
	// What arrived for this call before its id did.
	const auto early = std::exchange(_earlySignals, {});
	for (const auto &signal : early) {
		handleSignal(signal);
	}
	schedulePoll();
}

void Call::startFailed(const Api::Error &error) {
	_told = true; // there is no call to end
	if (_state == State::Ended) {
		return;
	}
	if (_video
		&& !_videoFallbackTried
		&& (error.code == u"call.video_disabled")) {
		// Video calls were switched off since client-config was read: the
		// person is called all the same, with audio.
		_videoFallbackTried = true;
		_told = false;
		setVideo(false);
		if (_cameraWanted) {
			_cameraWanted = false;
			Q_EMIT cameraChanged(false);
		}
		_pendingStart = std::make_shared<PendingStart>();
		sendStart();
		return;
	}
	_startError = error;
	end(EndReasonFromStartError(error));
}

void Call::ring(const QString &call, bool video) {
	if (_started) {
		return;
	}
	_started = true;
	_outgoing = false;
	_video = video;
	_id = call;
	_state = State::Incoming;
	// The owner learned about the call before it knew what it is.
	Q_EMIT stateChanged(_state);
	// The caller's screen moves from «calling» to «ringing» on the first
	// report; repeating it is harmless.
	_args.api->command(_id, u"ringing"_s);
	_ringTimer.callOnce(_args.timings.ringTimeoutMs);
	// At once: the call may be over already (a signal that was late).
	poll();
}

void Call::resume(const Data::ActiveCall &call) {
	if (_started) {
		return;
	}
	_started = true;
	_outgoing = call.caller;
	_video = call.video;
	_id = call.call;
	_accepted = true;
	_state = State::Reconnecting;
	Q_EMIT stateChanged(_state);
	// As after a dropped connection: fresh credentials, the same budget.
	rejoin();
	schedulePoll();
}

// ---- the user's actions

void Call::answer(bool withCamera) {
	if (_state != State::Incoming) {
		return;
	}
	if (withCamera && !_cameraWanted) {
		_cameraWanted = true;
		Q_EMIT cameraChanged(true);
	}
	_ringTimer.cancel();
	setState(State::Connecting);
	_args.api->accept(_id, this, [this](const Data::CallJoin &join) {
		if (_state == State::Ended) {
			return;
		}
		_accepted = true;
		if (join.video) {
			setVideo(true);
		}
		joinMedia(join);
	}, [this](const Api::Error &error) {
		// Refused: there is nothing of mine to end on the server.
		_told = true;
		if (_state == State::Ended) {
			return;
		}
		if (error.code == u"call.answered_elsewhere") {
			end(EndReason::AnsweredElsewhere, true);
		} else if (error.notFound() || error.code == u"call.state") {
			// It ended while the answer was on its way.
			end(EndReason::Missed, true);
		} else {
			_startError = error;
			end(EndReason::Failed);
		}
	});
}

void Call::hangup() {
	if (_state == State::Ended) {
		return;
	}
	if (_id.isEmpty()) {
		// The start request may still be on its way: what it creates is
		// cancelled as soon as it exists.
		_pendingStart->abandoned = true;
		end(EndReason::Hangup, true);
		return;
	}
	const auto was = _state;
	tellServer(was);
	if (was == State::Incoming) {
		end(EndReason::DeclinedByMe, true);
	} else {
		end(EndReason::Hangup);
	}
}

void Call::setMuted(bool muted) {
	if (_muted == muted) {
		return;
	}
	_muted = muted;
	if (_media) {
		_media->setMuted(muted);
	}
	Q_EMIT mutedChanged(muted);
}

void Call::setCameraEnabled(bool enabled) {
	if (_state == State::Ended || _cameraWanted == enabled) {
		return;
	}
	_cameraWanted = enabled;
	if (_media) {
		// cameraChanged() comes from the media (or cameraFailed()).
		const auto was = _media->cameraEnabled();
		_media->setCameraEnabled(enabled);
		if (!enabled && !was) {
			// It was only wanted so far.
			Q_EMIT cameraChanged(false);
		}
	} else {
		// Applied when the audio room is joined.
		Q_EMIT cameraChanged(enabled);
	}
}

void Call::setCameraDevice(const QByteArray &id) {
	_cameraDevice = id;
	_cameraDeviceSet = true;
	if (_media) {
		_media->setCameraDevice(id);
	}
}

void Call::tellServer(State was, Fn<void()> done) {
	if (_id.isEmpty() || _told) {
		if (done) {
			done();
		}
		return;
	}
	_told = true;
	const auto verb = (was == State::Incoming)
		? u"reject"_s
		: (was == State::Ringing || was == State::Calling)
		? u"cancel"_s
		: u"hangup"_s;
	// Unguarded: the request must go out also when this object is destroyed
	// right after (the callbacks hold nothing of it).
	_args.api->command(_id, verb, base::Guard(), done, [=](const Api::Error &) {
		if (done) {
			done();
		}
	});
}

bool Call::shouldTellOnExit() const {
	// A call that only rings here just stops ringing (other devices of mine
	// may still answer it).
	return !_id.isEmpty()
		&& !_told
		&& _state != State::Ended
		&& _state != State::Incoming;
}

void Call::finishOnExit(Fn<void()> done) {
	if (!_started) {
		if (done) {
			done();
		}
		return;
	}
	if (_outgoing && _id.isEmpty() && !_pendingStart->answered) {
		// Not known to the server yet (or the answer is on its way): the
		// call is cancelled when it turns out to exist.
		_pendingStart->abandoned = true;
		_pendingStart->afterCancel = std::move(done);
		end(EndReason::Hangup, true);
		return;
	}
	if (!shouldTellOnExit()) {
		if (_state != State::Ended) {
			end((_state == State::Incoming) ? EndReason::Missed : EndReason::Hangup, true);
		}
		if (done) {
			done();
		}
		return;
	}
	const auto was = _state;
	tellServer(was, std::move(done));
	end(EndReason::Hangup, true);
}

// ---- the server's side

void Call::applyServerState(
		const QString &state,
		const QString &reason,
		qint64 durationS) {
	if (_state == State::Ended) {
		return;
	}
	if (state == u"accepted" || state == u"connected") {
		// ("connected": one side reported its audio; mine may still be on
		// its way.)
		if (_state == State::Incoming) {
			// Answered, and not by this device.
			_told = true;
			end(EndReason::AnsweredElsewhere, true);
			return;
		}
		_accepted = true;
		if (_state == State::Ringing || _state == State::Calling) {
			setState(State::Connecting);
		}
		if (_state == State::Connecting && _media && _media->isConnected()) {
			reportConnected();
			setState(State::InCall);
		}
	} else if (state == u"ended") {
		_told = true; // the server ended it: nothing to tell
		if (durationS >= 0) {
			_endedDuration = durationS;
		}
		if (_state == State::Incoming) {
			end(EndReason::Missed, true);
		} else {
			end(EndReasonFromServer(reason));
		}
	}
}

void Call::handleSignal(const Data::CallSignal &signal) {
	if (_state == State::Ended || !_started) {
		return;
	}
	if (_id.isEmpty()) {
		// Mine, perhaps: the signal was faster than the answer that names
		// the call.
		if (_outgoing && _earlySignals.size() < 16) {
			_earlySignals.push_back(signal);
		}
		return;
	}
	if (signal.call != _id) {
		return;
	}
	if (signal.video) {
		// Every signal of a video call says so (never `false`).
		setVideo(true);
	}
	switch (signal.type) {
	case Data::CallSignal::Type::Ended:
		applyServerState(u"ended"_s, signal.reason, -1);
		break;
	case Data::CallSignal::Type::AnsweredElsewhere:
		// Sent to every device of the callee, the answering one included
		// (which is past Incoming by then).
		if (_state == State::Incoming) {
			_told = true;
			end(EndReason::AnsweredElsewhere, true);
		}
		break;
	case Data::CallSignal::Type::Accepted:
		applyServerState(u"accepted"_s, QString(), -1);
		break;
	case Data::CallSignal::Type::Ringing:
		if (_state == State::Calling) {
			setState(State::Ringing);
		}
		break;
	case Data::CallSignal::Type::Incoming:
	case Data::CallSignal::Type::Unknown:
		break;
	}
}

void Call::signalsConnectedChanged(bool connected) {
	if (_signalsConnected == connected) {
		return;
	}
	_signalsConnected = connected;
	if (_state == State::Ended || _id.isEmpty()) {
		return;
	}
	// Signals may have been missed while it was down (and without them the
	// poll is what moves the call): ask now.
	_pollTimer.cancel();
	poll();
}

void Call::schedulePoll() {
	if (_state == State::Ended || _id.isEmpty()) {
		return;
	}
	const auto &timings = _args.timings;
	_pollTimer.callOnce((_state == State::InCall)
		? timings.pollInCallMs
		: _signalsConnected
		? timings.pollConnectedMs
		: timings.pollOfflineMs);
}

void Call::poll() {
	if (_state == State::Ended || _id.isEmpty()) {
		return;
	}
	if (_pollInFlight) {
		schedulePoll();
		return;
	}
	_pollInFlight = true;
	_args.api->get(_id, this, [this](const Data::CallInfo &info) {
		_pollInFlight = false;
		if (_state == State::Ended) {
			return;
		}
		if (info.video) {
			setVideo(true);
		}
		applyServerState(info.state, info.endReason, info.durationS);
		if (info.delivered && _state == State::Calling) {
			setState(State::Ringing);
		}
	}, [this](const Api::Error &error) {
		_pollInFlight = false;
		if (error.notFound()) {
			applyServerState(u"ended"_s, QString(), -1); // the call is gone
		}
	});
	schedulePoll();
}

// ---- the audio

void Call::joinMedia(const Data::CallJoin &join) {
	if (!_media) {
		_media = _args.createMedia ? _args.createMedia() : nullptr;
		if (!_media) {
			// No audio on this machine: a call nobody can hear is no call.
			const auto was = _state;
			tellServer(was);
			end(EndReason::Failed);
			return;
		}
		// Chosen while it was still ringing.
		_media->setMuted(_muted);
		connect(_media.get(), &Media::stateChanged, this, [this](
				Media::State state) {
			mediaStateChanged(state);
		});
		connect(_media.get(), &Media::statsReady, this, [this](
				const MediaStats &stats) {
			sendQos(stats);
		});
		connect(
			_media.get(),
			&Media::levelsChanged,
			this,
			&Call::levelsChanged);
		connect(
			_media.get(),
			&Media::deviceFallback,
			this,
			&Call::deviceFallback);
		connect(_media.get(), &Media::inputFailed, this, &Call::inputFailed);
		connect(_media.get(), &Media::cameraChanged, this, [this](bool enabled) {
			if (_state == State::Ended) {
				return;
			} else if (!enabled && _cameraWanted) {
				// It went off by itself (unplugged).
				_cameraWanted = false;
			}
			Q_EMIT cameraChanged(enabled);
		});
		connect(_media.get(), &Media::cameraFailed, this, [this](bool noDevice) {
			if (_state == State::Ended) {
				return;
			}
			// The call goes on with audio.
			_cameraWanted = false;
			Q_EMIT cameraChanged(false);
			Q_EMIT cameraFailed(noDevice);
		});
		connect(_media.get(), &Media::remoteVideoChanged, this, [this](bool shown) {
			if (_state != State::Ended && _remoteVideo != shown) {
				_remoteVideo = shown;
				Q_EMIT remoteVideoChanged(shown);
			}
		});
		connect(
			_media.get(),
			&Media::localVideoFrame,
			this,
			&Call::localVideoFrame);
		connect(
			_media.get(),
			&Media::remoteVideoFrame,
			this,
			&Call::remoteVideoFrame);
		if (_cameraDeviceSet) {
			_media->setCameraDevice(_cameraDevice);
		}
		if (_cameraWanted) {
			// Asked for before there was a media (a video call I started, an
			// answer with the camera).
			_media->setCameraEnabled(true);
		}
	}
	const auto media = MediaJoin::FromJson(join.media);
	if (!media.valid()) {
		// The server let this device in without saying where to: treated
		// like a connection that failed at once.
		rejoin();
		return;
	}
	_media->join(media);
}

void Call::mediaStateChanged(Media::State state) {
	if (_state == State::Ended) {
		return;
	}
	switch (state) {
	case Media::State::Connected:
		mediaConnected();
		break;
	case Media::State::Reconnecting:
		// Only a call that was up can be "reconnecting" (not one that
		// still rings there). The library's own retries count against the
		// same budget.
		if (_state == State::InCall) {
			setState(State::Reconnecting);
			startReconnectClock();
		}
		break;
	case Media::State::Failed:
		rejoin();
		break;
	case Media::State::Connecting:
	case Media::State::Idle:
		break;
	}
}

void Call::mediaConnected() {
	stopReconnectClock();
	if (!_accepted) {
		return; // the caller's audio is ready before the answer
	}
	reportConnected();
	if (_state == State::Connecting || _state == State::Reconnecting) {
		setState(State::InCall);
		schedulePoll();
	}
}

void Call::reportConnected() {
	if (_id.isEmpty() || _connectedReported) {
		return;
	}
	_connectedReported = true;
	_args.api->command(_id, u"connected"_s);
}

void Call::startReconnectClock() {
	if (_reconnectClock.isValid()) {
		return;
	}
	_reconnectClock.start();
	_rejoinTries = 0;
	_reconnectWatchdog.callOnce(_args.timings.reconnectBudgetMs);
}

void Call::stopReconnectClock() {
	_reconnectClock.invalidate();
	_rejoinTries = 0;
	_reconnectWatchdog.cancel();
	_rejoinTimer.cancel();
}

void Call::failCall() {
	if (_state == State::Ended) {
		return;
	}
	tellServer(_state);
	end(EndReason::Failed);
	Q_EMIT reconnectFailed();
}

void Call::rejoin() {
	_rejoinTimer.cancel();
	if (_id.isEmpty() || _state == State::Ended || _rejoinInFlight) {
		return;
	}
	startReconnectClock();
	const auto budget = _args.timings.reconnectBudgetMs;
	if (_reconnectClock.elapsed() >= budget) {
		failCall();
		return;
	}
	// A call that still rings there keeps its ringing screen while the
	// audio room is joined again.
	if (_state == State::InCall || _state == State::Connecting) {
		setState(State::Reconnecting);
	}
	_rejoinInFlight = true;
	_args.api->rejoin(_id, this, [this](const Data::CallJoin &join) {
		_rejoinInFlight = false;
		if (_state == State::Ended || !_reconnectClock.isValid()) {
			return;
		}
		if (!_media) {
			joinMedia(join);
			return;
		}
		const auto media = MediaJoin::FromJson(join.media);
		if (media.valid()) {
			_media->join(media);
		} else {
			_rejoinTimer.callOnce(_args.timings.rejoinDelayMs);
		}
	}, [this](const Api::Error &error) {
		_rejoinInFlight = false;
		if (_state == State::Ended) {
			return;
		}
		// "call.state": the call ended (the room was closed when the other
		// side hung up).
		if (error.notFound() || error.code == u"call.state") {
			applyServerState(u"ended"_s, QString(), -1);
			return;
		}
		const auto left = _reconnectClock.isValid()
			? (_args.timings.reconnectBudgetMs - _reconnectClock.elapsed())
			: qint64(0);
		if (left <= 0) {
			failCall();
			return;
		}
		const auto delay = std::min(
			qint64(_args.timings.rejoinDelayMs) << std::min(_rejoinTries, 2),
			left);
		++_rejoinTries;
		_rejoinTimer.callOnce(int(delay));
	});
}

// ---- quality reports

void Call::scheduleQos(int delayMs) {
	if (delayMs > 0) {
		_qosTimer.callOnce(delayMs);
	}
}

void Call::sendQos(const MediaStats &stats) {
	if (!stats.valid || _state != State::InCall || _id.isEmpty()) {
		return;
	}
	auto qos = Data::CallQos();
	qos.rttMs = stats.rttMs;
	qos.jitterMs = stats.jitterMs;
	qos.lossPercent = stats.lossPercent;
	qos.mos = stats.mos;
	qos.relayTcp = stats.relayTcp;
	qos.netType = _args.networkType ? _args.networkType() : u"unknown"_s;
	qos.durationMs = _connectedAt
		? std::max(QDateTime::currentMSecsSinceEpoch() - _connectedAt, qint64(0))
		: 0;
	_args.api->qos(_id, qos);
}

} // namespace Calls
