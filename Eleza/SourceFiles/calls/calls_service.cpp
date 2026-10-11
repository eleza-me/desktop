#include "calls/calls_service.h"

#include "calls/calls_incoming_window.h"
#include "calls/calls_panel.h"
#include "calls/calls_sounds.h"
#include "calls/calls_text.h"
#include "core/core_application.h"
#include "data/data_calls.h"
#include "data/data_session.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_key_value.h"
#include "support/error_text.h"
#include "window/window_main_window.h"
#include "window/window_session_controller.h"

#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QMediaDevices>
#include <QtNetwork/QNetworkInformation>
#include <QtWidgets/QApplication>

namespace Calls {
namespace {

// How long an ended call stays on screen.
constexpr auto kEndedShownMs = 3500;
constexpr auto kFailedShownMs = 8000;

[[nodiscard]] QPointer<Service> &CurrentService() {
	static auto result = QPointer<Service>();
	return result;
}

[[nodiscard]] QString StoredCallKey(Main::Session *session) {
	// "<name>.<user id>": removed with the account when the user signs out.
	return u"calls.current."_s + session->userId();
}

[[nodiscard]] QString NetworkType() {
	static const auto loaded = QNetworkInformation::loadDefaultBackend();
	const auto information = loaded ? QNetworkInformation::instance() : nullptr;
	if (!information) {
		return u"unknown"_s;
	}
	switch (information->transportMedium()) {
	case QNetworkInformation::TransportMedium::Ethernet: return u"ethernet"_s;
	case QNetworkInformation::TransportMedium::WiFi: return u"wifi"_s;
	case QNetworkInformation::TransportMedium::Cellular: return u"cellular"_s;
	default: break;
	}
	return u"unknown"_s;
}

} // namespace

Service::Service(Main::Session *session)
: _session(session)
, _sounds(std::make_unique<Sounds>())
, _dismissTimer([this] { dismissEnded(); }) {
	CurrentService() = this;
	auto &data = session->data().calls();
	connect(&data, &Data::Calls::signalReceived, this, [this](
			const Data::CallSignal &signal) {
		if (_instance) {
			_instance->handleSignal(signal);
		}
	});
	connect(&data, &Data::Calls::signalsConnectedChanged, this, [this](
			bool connected) {
		if (_instance) {
			_instance->signalsConnectedChanged(connected);
		}
	});
	connect(
		&Core::App().settings(),
		&Core::Settings::changed,
		this,
		[this] { settingsChanged(); });
	const auto &settings = Core::App().settings().calls();
	_echoCancellation = settings.echoCancellation;
	_noiseSuppression = settings.noiseSuppression;
	settingsChanged();

	adopt(std::make_unique<Instance>(instanceArgs()));
	if (session->isReady()) {
		_instance->sessionReady();
	} else {
		connect(session, &Main::Session::ready, this, [this] {
			if (_instance) {
				_instance->sessionReady();
			}
		});
	}
#ifdef ELEZA_DEBUG_TOOLS
	startDebugControl();
#endif // ELEZA_DEBUG_TOOLS
}

Service::~Service() {
	_incoming = nullptr;
	if (_host) {
		delete _host.data();
	}
	// The call (if any) tells the server that this side is gone.
	_instance = nullptr;
}

Service *Service::For(Main::Session *session) {
	const auto current = CurrentService().data();
	return (current && current->_session == session) ? current : nullptr;
}

Service *Service::Current() {
	return CurrentService().data();
}

Instance::Args Service::instanceArgs() {
	auto result = Instance::Args();
	result.api = &_session->data().calls();
	result.createMedia = [this] { return CreateMedia(mediaSettings()); };
	result.checkMedia = [] { return CheckMedia(); };
	result.hasMicrophone = [] {
#ifdef ELEZA_DEBUG_TOOLS
		if (!qEnvironmentVariableIsEmpty("ELEZA_DEBUG_CALL_INPUT")) {
			return true;
		}
#endif // ELEZA_DEBUG_TOOLS
		return !QMediaDevices::audioInputs().isEmpty();
	};
	result.networkType = [] { return NetworkType(); };
#ifdef ELEZA_DEBUG_TOOLS
	// How often a device without the realtime connection asks whether a
	// call rings (a test does not wait a minute).
	if (const auto interval = qEnvironmentVariableIntValue(
			"ELEZA_DEBUG_CALL_INCOMING_INTERVAL_MS"); interval > 0) {
		result.incomingCheckIntervalMs = interval;
	}
#endif // ELEZA_DEBUG_TOOLS
	const auto session = _session;
	result.loadStoredCall = [=] {
		return session->account().settings().string(StoredCallKey(session));
	};
	result.storeCall = [=](const QString &id) {
		if (Core::App().commandLine().ephemeral()) {
			return; // a screenshot run leaves nothing behind
		}
		auto &settings = session->account().settings();
		if (id.isEmpty()) {
			settings.remove(StoredCallKey(session));
		} else {
			settings.set(StoredCallKey(session), id);
		}
	};
	return result;
}

MediaSettings Service::mediaSettings() const {
	const auto &settings = Core::App().settings().calls();
	auto result = MediaSettings();
	result.inputDeviceId = settings.inputDevice;
	result.outputDeviceId = settings.outputDevice;
	result.echoCancellation = settings.echoCancellation;
	result.noiseSuppression = settings.noiseSuppression;
	result.autoGainControl = true;
#ifdef ELEZA_DEBUG_TOOLS
	result.debugInput = qEnvironmentVariable("ELEZA_DEBUG_CALL_INPUT");
	result.debugOutput = qEnvironmentVariable("ELEZA_DEBUG_CALL_OUTPUT");
#endif // ELEZA_DEBUG_TOOLS
	return result;
}

void Service::adopt(std::unique_ptr<Instance> instance) {
	_dismissTimer.cancel();
	if (_instance) {
		_instance->disconnect(this);
	}
	_instance = std::move(instance);
	connect(_instance.get(), &Instance::callChanged, this, [this] {
		_dismissTimer.cancel();
		_wasInCall = false;
		watchCall();
		refresh();
		Q_EMIT callChanged();
	});
	connect(_instance.get(), &Instance::callUpdated, this, [this] {
		refresh();
		Q_EMIT callUpdated();
	});
	connect(_instance.get(), &Instance::callEnded, this, [this] {
		callEnded();
	});
	connect(_instance.get(), &Instance::notice, this, [this](Notice notice) {
		showNotice(notice);
	});
	watchCall();
	refresh();
	Q_EMIT callChanged();
}

#ifdef ELEZA_DEBUG_TOOLS
void Service::replaceInstance(std::unique_ptr<Instance> instance) {
	_wasInCall = false;
	_lastState = State::Ended;
	adopt(std::move(instance));
}
#endif // ELEZA_DEBUG_TOOLS

void Service::watchCall() {
	const auto call = this->call();
	if (_watched == call) {
		return;
	}
	if (_watched) {
		_watched->disconnect(this);
	}
	_watched = call;
	_inputLevel = _outputLevel = 0.;
	if (call) {
		connect(call, &Call::levelsChanged, this, [this](
				float input,
				float output) {
			_inputLevel = input;
			_outputLevel = output;
			Q_EMIT levelsChanged();
		});
	}
	Q_EMIT levelsChanged();
}

bool Service::engaged() const {
	const auto call = this->call();
	return call && !call->ended() && call->state() != State::Incoming;
}

void Service::refresh() {
	const auto call = this->call();
	const auto state = call ? call->state() : State::Ended;
	const auto live = call && !call->ended();
	if (live && state == State::Incoming) {
		if (_lastState != State::Incoming) {
			flashTaskbar();
		}
		_sounds->loop(Sounds::Loop::Ringtone);
	} else if (live && state == State::Ringing) {
		_sounds->loop(Sounds::Loop::Ringback);
	} else if (_sounds->looping() != Sounds::Loop::None) {
		_sounds->loop(Sounds::Loop::None);
	}
	if (live && state == State::InCall && !_wasInCall) {
		_wasInCall = true;
		_sounds->play(Sounds::Tone::Connect);
	}
	_lastState = live ? state : State::Ended;
	if (!call || (live && state != State::InCall && state != State::Reconnecting)) {
		if (_inputLevel > 0. || _outputLevel > 0.) {
			_inputLevel = _outputLevel = 0.;
			Q_EMIT levelsChanged();
		}
	}
	if (!call && _expanded) {
		_expanded = false;
		Q_EMIT expandedChanged();
	}
	updateIncomingWindow();
}

void Service::callEnded() {
	const auto call = this->call();
	if (!call) {
		return;
	}
	if (call->quiet()) {
		// It only rang here (or never came to exist): nothing to show.
		_sounds->loop(Sounds::Loop::None);
		return;
	}
	const auto reason = call->endReason();
	const auto refused = (reason == EndReason::Busy)
		|| (reason == EndReason::Declined);
	_sounds->play(refused ? Sounds::Tone::Busy : Sounds::Tone::End);
	const auto failed = refused
		|| (reason == EndReason::Failed)
		|| (reason == EndReason::NotAllowed)
		|| (reason == EndReason::NoAnswer)
		|| (reason == EndReason::Lost);
	_dismissTimer.callOnce(failed ? kFailedShownMs : kEndedShownMs);
	// A refusal the bar has no words for (too many calls, no connection).
	if (reason == EndReason::Failed
		&& call->startError()
		&& Support::ErrorPhrase(call->startError()->code).isEmpty()) {
		if (const auto window = Core::App().window()) {
			window->showToast(Support::ErrorText(
				*call->startError(),
				EndedText(*call)));
		}
	}
}

void Service::showNotice(Notice notice) {
	Q_EMIT noticeShown(notice);
	const auto window = Core::App().window();
	if (!window) {
		return;
	}
	if (notice == Notice::NoMicrophone || notice == Notice::MediaUnavailable) {
		// The answer to a click, possibly in the incoming call window while
		// the main one is hidden.
		window->activateAndShow();
	}
	window->showToast(NoticeText(notice));
}

void Service::settingsChanged() {
	const auto &settings = Core::App().settings();
	_sounds->setRingtoneVolume(settings.calls().ringtoneVolume);
	_sounds->setEnabled(settings.notifications().soundEnabled
		&& !Core::App().commandLine().ephemeral());
	const auto echo = settings.calls().echoCancellation;
	const auto noise = settings.calls().noiseSuppression;
	if (_echoCancellation != echo || _noiseSuppression != noise) {
		_echoCancellation = echo;
		_noiseSuppression = noise;
		if (const auto call = this->call(); call && call->media()) {
			call->media()->setProcessing(echo, noise, true);
		}
	}
}

void Service::setController(Window::SessionController *controller) {
	if (_controller == controller) {
		return;
	}
	if (_host) {
		delete _host.data();
	}
	_controller = controller;
	if (controller) {
		_host = new Host(this, controller);
	}
}

Window::SessionController *Service::controller() const {
	return _controller.data();
}

void Service::start(const QString &userId) {
	if (!_instance) {
		return;
	}
	const auto already = live() && (call()->peerId() == userId);
	if (_instance->start(userId) && already) {
		// «تماس» on somebody I am talking to: show that call.
		if (call()->state() != State::Incoming) {
			setExpanded(true);
		}
	}
}

void Service::answer() {
	if (!_instance) {
		return;
	}
	_instance->answer();
	if (const auto call = this->call();
		call && !call->ended() && call->state() != State::Incoming) {
		showWindow();
	}
}

void Service::hangup() {
	if (const auto call = this->call()) {
		call->hangup();
	}
}

void Service::callAgain() {
	const auto call = this->call();
	if (!call || !call->ended() || !_instance) {
		return;
	}
	const auto peer = call->peerId();
	_instance->start(peer);
}

void Service::toggleMuted() {
	if (const auto call = this->call(); call && !call->ended()) {
		call->setMuted(!call->muted());
	}
}

void Service::dismissEnded() {
	_dismissTimer.cancel();
	if (_instance) {
		_instance->dismissEnded();
	}
}

void Service::setInputDevice(const QByteArray &id) {
	auto settings = Core::App().settings().calls();
	settings.inputDevice = id;
	Core::App().settings().setCalls(settings);
	if (const auto call = this->call(); call && call->media()) {
		call->media()->setInputDevice(id);
	}
}

void Service::setOutputDevice(const QByteArray &id) {
	auto settings = Core::App().settings().calls();
	settings.outputDevice = id;
	Core::App().settings().setCalls(settings);
	if (const auto call = this->call(); call && call->media()) {
		call->media()->setOutputDevice(id);
	}
}

void Service::setExpanded(bool expanded) {
	if (expanded && !call()) {
		return;
	}
	if (_expanded != expanded) {
		_expanded = expanded;
		Q_EMIT expandedChanged();
	}
}

void Service::showWindow() {
	if (const auto window = Core::App().window()) {
		window->activateAndShow();
	}
}

void Service::updateIncomingWindow() {
	const auto call = this->call();
	const auto ringing = call
		&& !call->ended()
		&& (call->state() == State::Incoming);
	if (!ringing) {
		_incoming = nullptr;
		return;
	}
	if (_incoming && _incoming->callId() == call->id()) {
		return;
	}
	_incoming = std::make_unique<IncomingWindow>(
		_session,
		call->id(),
		call->peerId());
	_incoming->setAcceptCallback([this] { answer(); });
	_incoming->setDeclineCallback([this] { hangup(); });
	_incoming->showAtCorner();
}

void Service::flashTaskbar() {
	const auto window = Core::App().window();
	if (window
		&& Core::App().settings().notifications().flashTaskbar
		&& !Core::App().windowActive()) {
		QApplication::alert(window, 0);
	}
}

void Service::finishOnExit(Fn<void()> done) {
	_sounds->stop();
	_incoming = nullptr;
	if (_instance) {
		_instance->finishOnExit(std::move(done));
	} else if (done) {
		done();
	}
}

} // namespace Calls
