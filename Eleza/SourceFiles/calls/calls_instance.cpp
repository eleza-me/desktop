#include "calls/calls_instance.h"

namespace Calls {

Instance::Instance(Args args, QObject *parent)
: QObject(parent)
, _args(std::move(args))
, _incomingTimer([this] { checkIncoming(false); }) {
	Q_ASSERT(_args.api != nullptr);
	qRegisterMetaType<Calls::Notice>();
	_signalsConnected = _args.api->signalsConnected();
}

Instance::~Instance() = default;

Call::Args Instance::callArgs(const QString &peerId, bool video) const {
	auto result = Call::Args();
	result.video = video;
	result.api = _args.api;
	result.createMedia = _args.createMedia;
	result.networkType = _args.networkType;
	result.peerId = peerId;
	result.timings = _args.timings;
	return result;
}

void Instance::sessionReady() {
	if (_ready) {
		return;
	}
	_ready = true;
	recover();
	updateIncomingTimer();
}

void Instance::recover() {
	const auto stored = _args.loadStoredCall ? _args.loadStoredCall() : QString();
	if (stored.isEmpty()) {
		checkIncoming(true);
		return;
	}
	// The app went away in the middle of a call. Whether that call still
	// exists - and is the one the user is in, not a call on their phone -
	// only the server knows.
	_args.api->active(this, [=, this](const std::optional<Data::ActiveCall> &active) {
		if (live()) {
			return; // something happened meanwhile
		}
		if (!active || active->call != stored) {
			if (_args.storeCall) {
				_args.storeCall(QString());
			}
			checkIncoming(true);
			return;
		}
		if (active->state == u"accepted" || active->state == u"connected") {
			auto call = std::make_unique<Call>(callArgs(active->peer));
			const auto raw = call.get();
			setCall(std::move(call));
			raw->resume(*active);
			Q_EMIT callUpdated();
		} else {
			// It was only ringing: nobody is waiting for the answer here
			// any more (mine), or it simply rings again (theirs).
			if (_args.storeCall) {
				_args.storeCall(QString());
			}
			if (active->caller) {
				_args.api->command(active->call, u"cancel"_s);
			} else {
				checkIncoming(true);
			}
		}
	}, [this](const Api::Error &) {
		// Not now: the stored id stays for the next start.
		checkIncoming(true);
	});
}

void Instance::setCall(std::unique_ptr<Call> call) {
	if (_call) {
		_call->disconnect(this);
		// May be inside one of its own signals.
		_call.release()->deleteLater();
	}
	_call = std::move(call);
	if (_call) {
		watch(_call.get());
	}
	updateIncomingTimer();
	Q_EMIT callChanged();
}

void Instance::watch(Call *call) {
	connect(call, &Call::stateChanged, this, [=, this] {
		callStateChanged(call);
	});
	connect(call, &Call::mutedChanged, this, &Instance::callUpdated);
	connect(call, &Call::idAssigned, this, [this](const QString &id) {
		if (_args.storeCall) {
			_args.storeCall(id);
		}
		// Mine exists, so theirs (if any) was refused by the server.
		_waitingIncoming = std::nullopt;
		Q_EMIT callUpdated();
	});
	connect(call, &Call::reconnectFailed, this, [this] {
		Q_EMIT notice(Notice::ReconnectFailed);
	});
	connect(call, &Call::inputFailed, this, [this] {
		Q_EMIT notice(Notice::MicrophoneFailed);
	});
	connect(call, &Call::videoChanged, this, &Instance::callUpdated);
	connect(call, &Call::cameraChanged, this, &Instance::callUpdated);
	connect(call, &Call::remoteVideoChanged, this, &Instance::callUpdated);
	connect(call, &Call::cameraFailed, this, [this](bool noDevice) {
		Q_EMIT notice(noDevice ? Notice::NoCamera : Notice::CameraFailed);
	});
	connect(call, &Call::deviceFallback, this, [this](bool input) {
		Q_EMIT notice(input
			? Notice::InputDeviceChanged
			: Notice::OutputDeviceChanged);
	});
}

void Instance::callStateChanged(Call *call) {
	if (call != _call.get()) {
		return;
	}
	if (!call->ended()) {
		if (!call->outgoing()
			&& call->state() == State::Connecting
			&& _args.storeCall) {
			// Answered here: this device is in the call from now on.
			_args.storeCall(call->id());
		}
		Q_EMIT callUpdated();
		return;
	}
	if (!call->id().isEmpty()) {
		_endedCall = call->id();
	}
	if (_args.storeCall) {
		_args.storeCall(QString());
	}
	if (call->endReason() == EndReason::AnsweredElsewhere) {
		Q_EMIT notice(Notice::AnsweredElsewhere);
	}
	Q_EMIT callEnded();
	if (call != _call.get()) {
		return; // a listener started the next call
	}
	const auto waiting = std::exchange(_waitingIncoming, std::nullopt);
	if (waiting && call->outgoing() && call->id().isEmpty()) {
		// Mine never came to exist; theirs is ringing here.
		ring(waiting->call, waiting->from, waiting->video);
		return;
	}
	if (call->quiet()) {
		setCall(nullptr);
	} else {
		updateIncomingTimer();
		Q_EMIT callUpdated();
	}
}

bool Instance::start(const QString &peerId, bool video) {
	if (peerId.isEmpty()) {
		return false;
	}
	if (live()) {
		if (_call->peerId() == peerId) {
			return true;
		}
		Q_EMIT notice(Notice::AlreadyInCall);
		return false;
	}
	if (_args.checkMedia && _args.checkMedia() != MediaProblem::None) {
		Q_EMIT notice(Notice::MediaUnavailable);
		return false;
	}
	if (_args.hasMicrophone && !_args.hasMicrophone()) {
		Q_EMIT notice(Notice::NoMicrophone);
		return false;
	}
	_waitingIncoming = std::nullopt;
	auto call = std::make_unique<Call>(callArgs(peerId, video));
	const auto raw = call.get();
	setCall(std::move(call));
	raw->start();
	return true;
}

void Instance::answer(bool withCamera) {
	if (!_call || _call->state() != State::Incoming) {
		return;
	}
	if (_args.checkMedia && _args.checkMedia() != MediaProblem::None) {
		Q_EMIT notice(Notice::MediaUnavailable);
		return; // it keeps ringing: another device can answer it
	}
	if (_args.hasMicrophone && !_args.hasMicrophone()) {
		Q_EMIT notice(Notice::NoMicrophone);
		return;
	}
	_call->answer(withCamera);
}

void Instance::dismissEnded() {
	if (_call && _call->ended()) {
		setCall(nullptr);
	}
}

void Instance::ring(const QString &call, const QString &from, bool video) {
	if (call.isEmpty() || from.isEmpty() || call == _endedCall) {
		// The last one is over here (declined, say), whatever an answer
		// that was already on its way says.
		return;
	}
	auto created = std::make_unique<Call>(callArgs(from));
	const auto raw = created.get();
	setCall(std::move(created));
	raw->ring(call, video);
}

void Instance::handleSignal(const Data::CallSignal &signal) {
	if (signal.type == Data::CallSignal::Type::Incoming) {
		if (!live()) {
			ring(signal.call, signal.from, signal.video);
		} else if (_call->id() == signal.call) {
			// A repeat of what rings already.
		} else if (_call->outgoing() && _call->id().isEmpty()) {
			_waitingIncoming = signal;
		}
		// Otherwise: busy. The caller was told so by the server.
		return;
	}
	if (_waitingIncoming
		&& _waitingIncoming->call == signal.call
		&& signal.type == Data::CallSignal::Type::Ended) {
		_waitingIncoming = std::nullopt;
	}
	if (_call) {
		_call->handleSignal(signal);
	}
}

void Instance::signalsConnectedChanged(bool connected) {
	if (_signalsConnected == connected) {
		return;
	}
	_signalsConnected = connected;
	if (_call) {
		_call->signalsConnectedChanged(connected);
	}
	if (connected && _ready) {
		// A call that started while this device was offline still rings.
		checkIncoming(false);
	}
	updateIncomingTimer();
}

void Instance::updateIncomingTimer() {
	// While signals cannot arrive, asking is the only way to hear a call.
	const auto wanted = _ready && !_signalsConnected && !live();
	if (wanted && !_incomingTimer.isActive()) {
		_incomingTimer.callEach(_args.incomingCheckIntervalMs);
	} else if (!wanted) {
		_incomingTimer.cancel();
	}
}

void Instance::checkIncoming(bool force) {
	if (live() || _incomingCheckInFlight) {
		return;
	}
	// A connection that comes and goes must not turn into a poll.
	if (!force
		&& _lastIncomingCheck.isValid()
		&& _lastIncomingCheck.elapsed() < _args.incomingCheckIntervalMs - 50) {
		return;
	}
	_lastIncomingCheck.start();
	_incomingCheckInFlight = true;
	_args.api->incoming(this, [this](const std::optional<Data::CallInfo> &info) {
		_incomingCheckInFlight = false;
		if (info && !live()) {
			ring(info->call, info->peer, info->video);
		}
	}, [this](const Api::Error &) {
		// An older server without the route, or offline again: nothing rings.
		_incomingCheckInFlight = false;
	});
}

void Instance::finishOnExit(Fn<void()> done) {
	if (!_call) {
		if (done) {
			done();
		}
		return;
	}
	if (_args.storeCall && live()) {
		// Ended on purpose: nothing to come back to.
		_args.storeCall(QString());
	}
	_call->finishOnExit(std::move(done));
}

} // namespace Calls
