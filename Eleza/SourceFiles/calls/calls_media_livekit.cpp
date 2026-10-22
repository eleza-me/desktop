// Calls::Media over the media library (calls_ffi.h).
//
// One engine for the process, on a thread of its own ("the media thread"):
// it owns the audio devices and makes every call into the library. The Media
// objects the app holds are thin: they post commands to the engine and get
// its signals back on their own thread. The engine serves one session (one
// call) at a time; a command or an event of another session is ignored.
//
// What a session is made of, in the library's terms:
//
//   audio source (48 kHz mono, fed 10 ms frames) -> local track -> published
//   room <- connect(url, token, ice servers)
//   remote track -> audio stream (48 kHz mono frames) -> Audio::Output
//   audio processing module: echo canceller (reference = what Audio::Output
//       played), noise suppression, gain control, on the captured frames
//
// and, while a camera is on (either side's, in a call of either kind):
//
//   video source (fed RGBA frames of the camera) -> local track, source
//       "camera" -> published; unpublished again when the camera goes off
//   remote video track -> video stream (RGBA frames) -> the call's view
//
// The camera itself is opened by the thin Media object on its own thread
// (calls_video.h); the engine only gets its frames.
//
// Message and field numbers are those of the protocol files of the pinned
// library version (livekit-ffi/protocol/*.proto, see cmake/eleza_livekit.cmake).
#include "calls/calls_media.h"

#include "calls/calls_audio_io.h"
#include "calls/calls_ffi.h"
#include "calls/calls_video.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QTransform>
#include <QtGui/QImage>
#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QSemaphore>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtMultimedia/QMediaDevices>

#include <atomic>
#include <cstring>

namespace Calls {
namespace {

using Ffi::Reader;
using Ffi::Writer;

// FfiRequest / FfiResponse / FfiEvent fields (ffi.proto).
namespace Request {
constexpr auto kConnect = 3;
constexpr auto kDisconnect = 4;
constexpr auto kPublishTrack = 5;
constexpr auto kUnpublishTrack = 6;
constexpr auto kGetSessionStats = 12;
constexpr auto kCreateVideoTrack = 15;
constexpr auto kCreateAudioTrack = 16;
constexpr auto kLocalTrackMute = 17;
constexpr auto kNewVideoStream = 20;
constexpr auto kNewVideoSource = 21;
constexpr auto kCaptureVideoFrame = 22;
constexpr auto kNewAudioStream = 25;
constexpr auto kNewAudioSource = 26;
constexpr auto kCaptureAudioFrame = 27;
constexpr auto kNewApm = 50;
constexpr auto kApmProcessStream = 51;
constexpr auto kApmProcessReverseStream = 52;
constexpr auto kApmSetStreamDelay = 53;
} // namespace Request

namespace Response {
constexpr auto kConnect = 3;
constexpr auto kDisconnect = 4;
constexpr auto kPublishTrack = 5;
constexpr auto kGetSessionStats = 12;
constexpr auto kCreateVideoTrack = 15;
constexpr auto kCreateAudioTrack = 16;
constexpr auto kNewVideoStream = 20;
constexpr auto kNewVideoSource = 21;
constexpr auto kNewAudioStream = 25;
constexpr auto kNewAudioSource = 26;
constexpr auto kNewApm = 49;
} // namespace Response

namespace Event {
constexpr auto kRoom = 1;
constexpr auto kVideoStream = 3;
constexpr auto kAudioStream = 4;
constexpr auto kConnect = 5;
constexpr auto kDisconnect = 7;
constexpr auto kPublishTrack = 9;
constexpr auto kGetSessionStats = 19;
constexpr auto kPanic = 20;
} // namespace Event

// RoomEvent.message (room.proto).
namespace Room {
constexpr auto kParticipantConnected = 2;
constexpr auto kTrackPublished = 7;
constexpr auto kTrackSubscribed = 9;
constexpr auto kTrackUnsubscribed = 10;
constexpr auto kTrackMuted = 12;
constexpr auto kTrackUnmuted = 13;
constexpr auto kDisconnected = 22;
constexpr auto kReconnecting = 23;
constexpr auto kReconnected = 24;
constexpr auto kDataPacketReceived = 27;
constexpr auto kByteStreamOpened = 34;
constexpr auto kTextStreamOpened = 35;
} // namespace Room

constexpr auto kTrackKindAudio = 1;
constexpr auto kTrackKindVideo = 2;
constexpr auto kTrackSourceCamera = 1;
constexpr auto kTrackSourceMicrophone = 2;
constexpr auto kVideoBufferRgba = 0;
// Frames of the other side's camera handed to the view per second, at most.
constexpr auto kRemoteVideoMinIntervalMs = 40;
// A picture larger than this is not what a call sends: not copied.
constexpr auto kMaxRemoteVideoSide = 4096;
constexpr auto kVideoPublishRetryMs = 3000;
constexpr auto kIceTransportAll = 2;
constexpr auto kGatherContinually = 1;

constexpr auto kConnectFailureDelayMs = 2000;
constexpr auto kConnectWatchdogMs = 25000;
constexpr auto kLevelsEveryFrames = 8; // 80 ms

// The first field `field` of a message, as a sub-reader.
[[nodiscard]] Reader Sub(Reader reader, int field) {
	while (reader.next()) {
		if (reader.field() == field && reader.isChunk()) {
			return reader.sub();
		}
	}
	return Reader();
}

[[nodiscard]] quint64 Varint(Reader reader, int field) {
	while (reader.next()) {
		if (reader.field() == field && !reader.isChunk()) {
			return reader.varint();
		}
	}
	return 0;
}

[[nodiscard]] QString String(Reader reader, int field) {
	while (reader.next()) {
		if (reader.field() == field && reader.isChunk()) {
			return reader.string();
		}
	}
	return QString();
}

// Owned* messages: { FfiOwnedHandle handle = 1 { id = 1 }; info = 2 }.
[[nodiscard]] quint64 OwnedHandle(Reader owned) {
	return Varint(Sub(owned, 1), 1);
}

struct SessionStats {
	bool have = false;
	double rtt = -1.;       // seconds
	double pairRtt = -1.;
	double jitter = -1.;
	quint64 received = 0;
	qint64 lost = 0;
	QString selectedLocal;
	QHash<QString, bool> localRelayTcp;
};

void ParseRtcStats(Reader stats, SessionStats &out) {
	while (stats.next()) {
		if (!stats.isChunk()) {
			continue;
		}
		const auto entry = stats.sub();
		switch (stats.field()) {
		case 4: { // InboundRtp { rtc, stream, received, inbound }
			if (String(Sub(entry, 2), 2) != QLatin1StringView("audio")) {
				break;
			}
			auto received = Sub(entry, 3);
			while (received.next()) {
				switch (received.field()) {
				case 1: out.received += received.varint(); break;
				case 2: out.lost += received.integer(); break;
				case 3:
					out.jitter = std::max(out.jitter, received.real());
					break;
				}
			}
			out.have = true;
		} break;
		case 6: { // RemoteInboundRtp { rtc, stream, received, remote_inbound }
			auto remote = Sub(entry, 4);
			auto measurements = quint64();
			auto rtt = -1.;
			while (remote.next()) {
				switch (remote.field()) {
				case 2: rtt = remote.real(); break;
				case 5: measurements = remote.varint(); break;
				}
			}
			if (measurements > 0 && rtt >= 0.) {
				out.rtt = std::max(out.rtt, rtt);
				out.have = true;
			}
		} break;
		case 13: { // CandidatePair { rtc, candidate_pair }
			auto pair = Sub(entry, 2);
			auto local = QString();
			auto nominated = false;
			auto state = -1;
			auto rtt = -1.;
			while (pair.next()) {
				switch (pair.field()) {
				case 2: local = pair.string(); break;
				case 4: state = int(pair.varint()); break;
				case 5: nominated = pair.boolean(); break;
				case 13: rtt = pair.real(); break;
				}
			}
			if (nominated && state == 4) { // PAIR_SUCCEEDED
				out.selectedLocal = local;
				if (rtt > 0.) {
					out.pairRtt = std::max(out.pairRtt, rtt);
				}
			}
		} break;
		case 14: { // LocalCandidate { rtc, candidate }
			const auto id = String(Sub(entry, 1), 1);
			auto candidate = Sub(entry, 2);
			auto type = -1;
			auto relayProtocol = -1;
			while (candidate.next()) {
				switch (candidate.field()) {
				case 5: type = int(candidate.varint()); break;
				case 8: relayProtocol = int(candidate.varint()); break;
				}
			}
			// RELAY over TCP or TLS.
			out.localRelayTcp.insert(
				id,
				(type == 3) && (relayProtocol == 1 || relayProtocol == 2));
		} break;
		}
	}
}

class Engine final : public QObject {
	Q_OBJECT

public:
	explicit Engine(Ffi::Library *library)
	: _library(library) {
	}

	// ---- commands (the media thread)
	void open(
		quint64 session,
		const MediaSettings &settings,
		const QAudioDevice &input,
		const QAudioDevice &output);
	void join(quint64 session, const MediaJoin &join);
	void leave(quint64 session);
	void close(quint64 session);
	void setMuted(quint64 session, bool muted);
	void setInput(quint64 session, const QAudioDevice &device);
	void setOutput(quint64 session, const QAudioDevice &device);
	void setProcessing(quint64 session, bool echo, bool noise, bool gain);
	void requestStats(quint64 session);
	// The camera of this side: published while `enabled` (now, or once the
	// room is joined); its frames come through captureVideo().
	void setCamera(quint64 session, bool enabled);
	void captureVideo(quint64 session, const QImage &rgba);
	void shutdown();

	void handleEvent(const QByteArray &bytes);

Q_SIGNALS:
	void stateChanged(quint64 session, int state);
	void levelsChanged(quint64 session, float input, float output);
	void statsReady(quint64 session, const Calls::MediaStats &stats);
	void inputFailed(quint64 session);
	void remoteVideoChanged(quint64 session, bool shown);
	void remoteVideoFrame(quint64 session, const QImage &frame);

private:
	[[nodiscard]] QByteArray request(int field, const Writer &message) {
		return _library->request(field, message);
	}
	void drop(quint64 &handle) {
		if (handle) {
			_library->dropHandle(std::exchange(handle, quint64(0)));
		}
	}
	void setState(Media::State state);
	void ensureAudioObjects();
	void startInput();
	void startOutput();
	void stopAudio();
	void ensureSource();
	void recreateApm();
	void closeRoom();
	void disconnectRoom(quint64 room);
	void closeStream();
	void connectFailed();
	void connected(Reader result);
	void publish();
	void roomEvent(Reader event);
	void audioStreamEvent(Reader event);
	void videoStreamEvent(Reader event);
	void statsEvent(Reader event);
	void captured(qint16 *samples);
	void publishVideo(QSize size);
	void unpublishVideo();
	void unpublishTrack(const QString &sid);
	void closeVideoStream();
	void setRemoteVideoShown(bool shown);

	Ffi::Library *_library = nullptr;
	quint64 _session = 0;
	Media::State _state = Media::State::Idle;
	MediaSettings _settings;
	QAudioDevice _inputDevice;
	QAudioDevice _outputDevice;
	bool _muted = false;
	int _generation = 0;

	std::unique_ptr<Audio::Input> _input;
	std::unique_ptr<Audio::Output> _output;
	std::unique_ptr<QTimer> _watchdog;
	std::vector<qint16> _reference;
	int _framesSinceLevels = 0;
	float _inputLevel = 0.;
	bool _inputFailed = false;

	quint64 _source = 0;
	quint64 _apm = 0;
	quint64 _room = 0;
	quint64 _participant = 0;
	quint64 _track = 0;
	quint64 _remoteTrack = 0;
	QString _remoteTrackSid;
	quint64 _stream = 0;
	quint64 _connectAsync = 0;
	quint64 _publishAsync = 0;
	quint64 _statsAsync = 0;
	bool _published = false;

	// ---- video
	bool _cameraWanted = false;
	quint64 _videoSource = 0;
	QSize _videoSourceSize;
	quint64 _videoTrack = 0;
	quint64 _videoPublishAsync = 0;
	// A publish that was in flight when the camera went off: unpublished
	// as soon as it is done.
	quint64 _videoAbandonedAsync = 0;
	QString _videoPublicationSid;
	bool _videoPublished = false;
	quint64 _remoteVideoTrack = 0;
	QString _remoteVideoSid;
	quint64 _videoStream = 0;
	bool _remoteVideoMuted = false;
	bool _remoteVideoShown = false;
	qint64 _remoteVideoFrameMs = 0;
	qint64 _videoRetryAt = 0;

	// Rooms that are being left: the library needs the handle until it says
	// that the room is closed (async id -> room handle).
	QHash<quint64, quint64> _closing;

	// Totals of the last report: loss is counted between two reports.
	quint64 _lastReceived = 0;
	qint64 _lastLost = 0;

};

void Engine::setState(Media::State state) {
	if (_state != state) {
		_state = state;
		Q_EMIT stateChanged(_session, int(state));
	}
}

void Engine::ensureAudioObjects() {
	if (_input) {
		return;
	}
	_input = std::make_unique<Audio::Input>();
	_output = std::make_unique<Audio::Output>();
	_watchdog = std::make_unique<QTimer>();
	_watchdog->setSingleShot(true);
	connect(_watchdog.get(), &QTimer::timeout, this, [this] {
		if (_state == Media::State::Connecting) {
			connectFailed();
		}
	});
	_input->setFrameCallback([this](qint16 *samples) { captured(samples); });
	_input->setFailedCallback([this] {
		// The microphone went away under us: try the default one once.
		const auto fallback = QMediaDevices::defaultAudioInput();
		if (!fallback.isNull() && fallback.id() != _inputDevice.id()) {
			_inputDevice = fallback;
			startInput();
		} else {
			_inputFailed = true;
			Q_EMIT inputFailed(_session);
		}
	});
}

void Engine::startInput() {
	auto started = false;
#ifdef ELEZA_DEBUG_TOOLS
	if (!_settings.debugInput.isEmpty()) {
		started = _input->startGenerated(_settings.debugInput);
	} else
#endif // ELEZA_DEBUG_TOOLS
	{
		started = _input->start(_inputDevice);
		if (!started) {
			const auto fallback = QMediaDevices::defaultAudioInput();
			if (!fallback.isNull() && fallback.id() != _inputDevice.id()) {
				_inputDevice = fallback;
				started = _input->start(_inputDevice);
			}
		}
	}
	const auto failed = !started;
	if (failed && !_inputFailed) {
		Q_EMIT inputFailed(_session);
	}
	_inputFailed = failed;
}

void Engine::startOutput() {
#ifdef ELEZA_DEBUG_TOOLS
	if (!_settings.debugOutput.isEmpty()) {
		if (!_output->startRecording(_settings.debugOutput)) {
			qWarning("Calls: cannot record the output of the call.");
		}
		return;
	}
#endif // ELEZA_DEBUG_TOOLS
	if (!_output->start(_outputDevice)) {
		const auto fallback = QMediaDevices::defaultAudioOutput();
		if (fallback.isNull()
			|| fallback.id() == _outputDevice.id()
			|| !_output->start(fallback)) {
			qWarning("Calls: no audio output could be opened.");
		} else {
			_outputDevice = fallback;
		}
	}
}

void Engine::stopAudio() {
	if (_input) {
		_input->stop();
		_output->stop();
	}
	_reference.clear();
	_inputLevel = 0.;
	_framesSinceLevels = 0;
}

void Engine::ensureSource() {
	if (_source) {
		return;
	}
	// NewAudioSourceRequest { type = 1 (native: 0), options = 2, sample_rate = 3,
	// num_channels = 4, queue_size_ms = 5 }. No queue: 10 ms frames go
	// straight to the encoder. The options stay off: processing is done by
	// the module below, which knows the loudspeaker's signal.
	auto options = Writer();
	options.boolean(1, false).boolean(2, false).boolean(3, false);
	auto message = Writer();
	message.varint(1, 0)
		.message(2, options)
		.varint(3, Audio::kSampleRate)
		.varint(4, 1)
		.varint(5, 0);
	const auto response = request(Request::kNewAudioSource, message);
	_source = OwnedHandle(Sub(Sub(Reader(response), Response::kNewAudioSource), 1));
}

void Engine::recreateApm() {
	drop(_apm);
	if (!_settings.echoCancellation
		&& !_settings.noiseSuppression
		&& !_settings.autoGainControl) {
		return;
	}
	// NewApmRequest { echo_canceller = 1, gain_controller = 2,
	// high_pass_filter = 3, noise_suppression = 4 }.
	auto message = Writer();
	message.boolean(1, _settings.echoCancellation)
		.boolean(2, _settings.autoGainControl)
		.boolean(3, true)
		.boolean(4, _settings.noiseSuppression);
	const auto response = request(Request::kNewApm, message);
	_apm = OwnedHandle(Sub(Sub(Reader(response), Response::kNewApm), 1));
}

void Engine::open(
		quint64 session,
		const MediaSettings &settings,
		const QAudioDevice &input,
		const QAudioDevice &output) {
	if (_session && _session != session) {
		close(_session);
	}
	_session = session;
	_settings = settings;
	_inputDevice = input;
	_outputDevice = output;
	_muted = false;
	_inputFailed = false;
	_state = Media::State::Idle;
}

void Engine::join(quint64 session, const MediaJoin &join) {
	if (session != _session || !join.valid()) {
		return;
	}
	ensureAudioObjects();
	closeRoom();
	ensureSource();
	if (!_source) {
		qWarning("Calls: the media library gave no audio source.");
		connectFailed();
		return;
	}
	if (!_apm) {
		recreateApm();
	}
	if (!_output->active()) {
		startOutput();
	}
	if (!_input->active() && !_inputFailed) {
		startInput();
	}

	// ConnectRequest { url = 1, token = 2, options = 3 }.
	// RoomOptions { auto_subscribe = 1, adaptive_stream = 2, dynacast = 3,
	// rtc_config = 5 }; RtcConfig { ice_transport_type = 1,
	// continual_gathering_policy = 2, ice_servers = 3 }.
	//
	// The only remote end is the SFU, so no path to the other user exists
	// whatever the candidates are; the TURN servers of the join are offered
	// next to the SFU's own addresses, for networks that pass nothing else
	// (the same way the mobile apps join).
	auto rtc = Writer();
	rtc.varint(1, kIceTransportAll).varint(2, kGatherContinually);
	if (!join.turnUrls.isEmpty()) {
		auto server = Writer();
		for (const auto &url : join.turnUrls) {
			server.string(1, url);
		}
		server.string(2, join.turnUsername).string(3, join.turnCredential);
		rtc.message(3, server);
	}
	auto options = Writer();
	options.boolean(1, true)
		.boolean(2, false)
		.boolean(3, false)
		.message(5, rtc);
	auto message = Writer();
	message.string(1, join.url).string(2, join.token).message(3, options);
	const auto response = request(Request::kConnect, message);
	_connectAsync = Varint(Sub(Reader(response), Response::kConnect), 1);
	++_generation;
	if (!_connectAsync) {
		connectFailed();
		return;
	}
	_watchdog->start(kConnectWatchdogMs);
	setState(Media::State::Connecting);
}

void Engine::connectFailed() {
	_connectAsync = 0;
	_publishAsync = 0;
	if (_watchdog) {
		_watchdog->stop();
	}
	closeRoom();
	// Not at once: an SFU that cannot be reached must not be asked for
	// fresh credentials in a tight loop.
	const auto generation = ++_generation;
	const auto session = _session;
	if (_state != Media::State::Connecting) {
		setState(Media::State::Connecting);
	}
	QTimer::singleShot(kConnectFailureDelayMs, this, [=, this] {
		if (_generation == generation && _session == session) {
			setState(Media::State::Failed);
		}
	});
}

void Engine::closeStream() {
	drop(_stream);
	drop(_remoteTrack);
	_remoteTrackSid = QString();
}

void Engine::closeVideoStream() {
	drop(_videoStream);
	drop(_remoteVideoTrack);
	_remoteVideoSid = QString();
	_remoteVideoMuted = false;
	_remoteVideoFrameMs = 0;
	setRemoteVideoShown(false);
}

void Engine::setRemoteVideoShown(bool shown) {
	if (_remoteVideoShown != shown) {
		_remoteVideoShown = shown;
		Q_EMIT remoteVideoChanged(_session, shown);
	}
}

void Engine::closeRoom() {
	closeStream();
	closeVideoStream();
	// The camera track belongs to the room that is going away; a camera
	// that is still wanted is published again in the next one.
	_videoPublished = false;
	_videoPublishAsync = 0;
	_videoAbandonedAsync = 0;
	_videoPublicationSid = QString();
	_videoRetryAt = 0;
	drop(_videoTrack);
	_published = false;
	_publishAsync = 0;
	_statsAsync = 0;
	drop(_track);
	drop(_participant);
	if (_room) {
		disconnectRoom(std::exchange(_room, quint64(0)));
	}
	_lastReceived = 0;
	_lastLost = 0;
}

void Engine::disconnectRoom(quint64 room) {
	// DisconnectRequest { room_handle = 1 } -> { async_id = 1 }, and later
	// the event DisconnectCallback { async_id = 1 }. The handle is given
	// back only then: the library looks the room up while it closes it.
	auto message = Writer();
	message.varint(1, room);
	const auto response = request(Request::kDisconnect, message);
	const auto asyncId = Varint(Sub(Reader(response), Response::kDisconnect), 1);
	if (asyncId) {
		_closing.insert(asyncId, room);
	} else {
		_library->dropHandle(room);
	}
}

void Engine::leave(quint64 session) {
	if (session != _session) {
		return;
	}
	++_generation;
	_connectAsync = 0;
	if (_watchdog) {
		_watchdog->stop();
	}
	closeRoom();
	stopAudio();
	_inputFailed = false;
	setState(Media::State::Idle);
}

void Engine::close(quint64 session) {
	if (session != _session) {
		return;
	}
	leave(session);
	drop(_apm);
	drop(_source);
	drop(_videoSource);
	_videoSourceSize = QSize();
	_cameraWanted = false;
	_session = 0;
	_state = Media::State::Idle;
}

void Engine::setMuted(quint64 session, bool muted) {
	if (session != _session || _muted == muted) {
		return;
	}
	_muted = muted;
	if (_track && _published) {
		// LocalTrackMuteRequest { track_handle = 1, mute = 2 }: the other
		// side sees the track as muted, and nothing is sent.
		auto message = Writer();
		message.varint(1, _track).boolean(2, muted);
		[[maybe_unused]] const auto response = request(
			Request::kLocalTrackMute,
			message);
	}
}

void Engine::setCamera(quint64 session, bool enabled) {
	if (session != _session || _cameraWanted == enabled) {
		return;
	}
	_cameraWanted = enabled;
	if (!enabled) {
		unpublishVideo();
	}
	// On: the first frame publishes the track (its size is known then).
}

void Engine::publishVideo(QSize size) {
	if (!_room
		|| !_participant
		|| !_published
		|| _videoPublishAsync
		|| _videoAbandonedAsync
		|| _videoPublished
		|| QDateTime::currentMSecsSinceEpoch() < _videoRetryAt) {
		return;
	}
	if (!_videoSource || _videoSourceSize != size) {
		drop(_videoSource);
		// NewVideoSourceRequest { type = 1 (native: 0), resolution = 2
		// { width = 1, height = 2 }, is_screencast = 3 }.
		auto resolution = Writer();
		resolution.varint(1, quint64(size.width())).varint(2, quint64(size.height()));
		auto message = Writer();
		message.varint(1, 0).message(2, resolution).boolean(3, false);
		const auto response = request(Request::kNewVideoSource, message);
		_videoSource = OwnedHandle(
			Sub(Sub(Reader(response), Response::kNewVideoSource), 1));
		_videoSourceSize = size;
	}
	if (!_videoSource) {
		qWarning("Calls: the media library gave no video source.");
		return;
	}
	// CreateVideoTrackRequest { name = 1, source_handle = 2 }.
	auto create = Writer();
	create.string(1, QStringLiteral("camera")).varint(2, _videoSource);
	const auto created = request(Request::kCreateVideoTrack, create);
	_videoTrack = OwnedHandle(
		Sub(Sub(Reader(created), Response::kCreateVideoTrack), 1));
	if (!_videoTrack) {
		return;
	}
	// TrackPublishOptions { simulcast = 6, source = 7 }: one layer is all a
	// call of two needs; the source has to be the camera (the token allows
	// the microphone and the camera, nothing else).
	auto options = Writer();
	options.boolean(6, false).varint(7, kTrackSourceCamera);
	auto message = Writer();
	message.varint(1, _participant).varint(2, _videoTrack).message(3, options);
	const auto response = request(Request::kPublishTrack, message);
	_videoPublishAsync = Varint(Sub(Reader(response), Response::kPublishTrack), 1);
	if (!_videoPublishAsync) {
		drop(_videoTrack);
	}
}

void Engine::unpublishVideo() {
	if (_videoPublishAsync) {
		_videoAbandonedAsync = std::exchange(_videoPublishAsync, 0);
	}
	if (_videoPublished) {
		unpublishTrack(_videoPublicationSid);
	}
	_videoPublished = false;
	_videoPublicationSid = QString();
	drop(_videoTrack);
}

void Engine::unpublishTrack(const QString &sid) {
	if (!_participant || sid.isEmpty()) {
		return;
	}
	// UnpublishTrackRequest { local_participant_handle = 1, track_sid = 2,
	// stop_on_unpublish = 3 }: the other side sees the track go away.
	auto message = Writer();
	message.varint(1, _participant)
		.string(2, sid)
		.boolean(3, true);
	[[maybe_unused]] const auto response = request(
		Request::kUnpublishTrack,
		message);
}

void Engine::captureVideo(quint64 session, const QImage &rgba) {
	if (session != _session
		|| !_cameraWanted
		|| rgba.isNull()
		|| rgba.format() != QImage::Format_RGBA8888) {
		return;
	}
	if (!_videoPublished) {
		publishVideo(rgba.size());
		return;
	} else if (rgba.size() != _videoSourceSize) {
		// Another camera, another picture: a new source and track.
		unpublishVideo();
		publishVideo(rgba.size());
		return;
	}
	// CaptureVideoFrameRequest { source_handle = 1, buffer = 2, timestamp_us = 3,
	// rotation = 4 }; VideoBufferInfo { type = 1, width = 2, height = 3,
	// data_ptr = 4, stride = 6 }. The library converts the picture before it
	// returns: the image only has to live through the call.
	auto buffer = Writer();
	buffer.varint(1, kVideoBufferRgba)
		.varint(2, quint64(rgba.width()))
		.varint(3, quint64(rgba.height()))
		.varint(4, quint64(quintptr(rgba.constBits())))
		.varint(6, quint64(rgba.bytesPerLine()));
	auto message = Writer();
	message.varint(1, _videoSource)
		.message(2, buffer)
		.integer(3, QDateTime::currentMSecsSinceEpoch() * 1000)
		.varint(4, 0);
	[[maybe_unused]] const auto response = request(
		Request::kCaptureVideoFrame,
		message);
}

void Engine::setInput(quint64 session, const QAudioDevice &device) {
	if (session != _session) {
		return;
	}
	_inputDevice = device;
	_inputFailed = false;
	if (_input && (_input->active() || _room)) {
		startInput();
	}
}

void Engine::setOutput(quint64 session, const QAudioDevice &device) {
	if (session != _session) {
		return;
	}
	_outputDevice = device;
	if (_output && _output->active()) {
		startOutput();
	}
}

void Engine::setProcessing(quint64 session, bool echo, bool noise, bool gain) {
	if (session != _session) {
		return;
	}
	if (_settings.echoCancellation == echo
		&& _settings.noiseSuppression == noise
		&& _settings.autoGainControl == gain) {
		return;
	}
	_settings.echoCancellation = echo;
	_settings.noiseSuppression = noise;
	_settings.autoGainControl = gain;
	if (_source) {
		recreateApm();
	}
}

void Engine::requestStats(quint64 session) {
	if (session != _session) {
		return;
	}
	if (!_room || _state != Media::State::Connected) {
		Q_EMIT statsReady(session, MediaStats());
		return;
	}
	// GetSessionStatsRequest { room_handle = 1 }.
	auto message = Writer();
	message.varint(1, _room);
	const auto response = request(Request::kGetSessionStats, message);
	_statsAsync = Varint(Sub(Reader(response), Response::kGetSessionStats), 1);
	if (!_statsAsync) {
		Q_EMIT statsReady(session, MediaStats());
	}
}

void Engine::shutdown() {
	if (_session) {
		close(_session);
	}
	_input = nullptr;
	_output = nullptr;
	_watchdog = nullptr;
	_library->setEventHandler(nullptr);
	_library->dispose();
}

void Engine::handleEvent(const QByteArray &bytes) {
	auto event = Reader(bytes);
	while (event.next()) {
		if (!event.isChunk()) {
			continue;
		}
		auto body = event.sub();
		switch (event.field()) {
		case Event::kAudioStream: audioStreamEvent(body); break;
		case Event::kVideoStream: videoStreamEvent(body); break;
		case Event::kRoom: roomEvent(body); break;
		case Event::kConnect: {
			// ConnectCallback { async_id = 1, error = 2, result = 3 }.
			auto asyncId = quint64();
			auto error = QString();
			auto result = Reader();
			auto hasError = false;
			while (body.next()) {
				switch (body.field()) {
				case 1: asyncId = body.varint(); break;
				case 2: error = body.string(); hasError = true; break;
				case 3: result = body.sub(); break;
				}
			}
			const auto mine = (asyncId != 0) && (asyncId == _connectAsync);
			if (mine) {
				_connectAsync = 0;
			}
			if (hasError) {
				if (mine) {
					// The text may name the SFU: for a debug build's log only.
#ifdef ELEZA_DEBUG_TOOLS
					qInfo("Calls: connect failed: %s", qPrintable(error));
#else // ELEZA_DEBUG_TOOLS
					qInfo("Calls: connect failed.");
#endif // ELEZA_DEBUG_TOOLS
					connectFailed();
				}
			} else if (mine) {
				connected(result);
			} else {
				// A room of a call that was left meanwhile: close it.
				auto stale = Reader(result);
				auto room = quint64();
				while (stale.next()) {
					if (!stale.isChunk()) {
						continue;
					}
					if (stale.field() == 1) {
						room = OwnedHandle(stale.sub());
					} else if (stale.field() == 2) {
						_library->dropHandle(OwnedHandle(stale.sub()));
					}
				}
				if (room) {
					disconnectRoom(room);
				}
			}
		} break;
		case Event::kDisconnect:
			if (const auto room = _closing.take(Varint(body, 1))) {
				_library->dropHandle(room);
			}
			break;
		case Event::kPublishTrack: {
			// PublishTrackCallback { async_id = 1, error = 2, publication = 3 }.
			auto asyncId = quint64();
			auto hasError = false;
			auto publication = quint64();
			auto publicationSid = QString();
			while (body.next()) {
				switch (body.field()) {
				case 1: asyncId = body.varint(); break;
				case 2: hasError = true; break;
				case 3: {
					// OwnedTrackPublication { handle = 1, info = 2 { sid = 1 } }.
					const auto owned = body.sub();
					publication = OwnedHandle(owned);
					publicationSid = String(Sub(owned, 2), 1);
				} break;
				}
			}
			if (publication) {
				_library->dropHandle(publication);
			}
			if (asyncId && asyncId == _videoAbandonedAsync) {
				// The camera went off while this was being published.
				_videoAbandonedAsync = 0;
				if (!hasError) {
					unpublishTrack(publicationSid);
				}
				break;
			}
			if (asyncId && asyncId == _videoPublishAsync) {
				_videoPublishAsync = 0;
				if (hasError) {
					// The call goes on with audio; not tried again with
					// every frame.
					qInfo("Calls: publishing the camera failed.");
					drop(_videoTrack);
					_videoRetryAt = QDateTime::currentMSecsSinceEpoch()
						+ kVideoPublishRetryMs;
				} else {
					_videoPublished = true;
					_videoPublicationSid = publicationSid;
					if (!_cameraWanted) {
						// Turned off while it was being published.
						unpublishVideo();
					}
				}
				break;
			}
			if (asyncId && asyncId == _publishAsync) {
				_publishAsync = 0;
				if (hasError) {
					qInfo("Calls: publishing the microphone failed.");
					connectFailed();
				} else {
					_published = true;
					if (_muted) {
						_muted = false;
						setMuted(_session, true);
					}
					if (_watchdog) {
						_watchdog->stop();
					}
					setState(Media::State::Connected);
				}
			}
		} break;
		case Event::kGetSessionStats: statsEvent(body); break;
		case Event::kPanic:
			qWarning("Calls: the media library reported an internal error.");
			if (_session && _state != Media::State::Idle) {
				connectFailed();
			}
			break;
		}
	}
}

void Engine::connected(Reader result) {
	// Result { room = 1 (OwnedRoom), local_participant = 2 (OwnedParticipant),
	// participants = 3 { participant = 1, publications = 2 } }.
	while (result.next()) {
		if (!result.isChunk()) {
			continue;
		}
		switch (result.field()) {
		case 1: _room = OwnedHandle(result.sub()); break;
		case 2: _participant = OwnedHandle(result.sub()); break;
		case 3: {
			// Who is there already: their tracks arrive as events; the
			// handles are of no use here.
			auto other = result.sub();
			while (other.next()) {
				if (other.isChunk()) {
					_library->dropHandle(OwnedHandle(other.sub()));
				}
			}
		} break;
		}
	}
	if (!_room || !_participant) {
		connectFailed();
		return;
	}
	publish();
}

void Engine::publish() {
	// CreateAudioTrackRequest { name = 1, source_handle = 2 }.
	auto create = Writer();
	create.string(1, QStringLiteral("microphone")).varint(2, _source);
	const auto created = request(Request::kCreateAudioTrack, create);
	_track = OwnedHandle(Sub(Sub(Reader(created), Response::kCreateAudioTrack), 1));
	if (!_track) {
		connectFailed();
		return;
	}
	// PublishTrackRequest { local_participant_handle = 1, track_handle = 2,
	// options = 3 }; TrackPublishOptions { dtx = 4, red = 5, source = 7 }.
	auto options = Writer();
	options.boolean(4, true)
		.boolean(5, true)
		.varint(7, kTrackSourceMicrophone);
	auto message = Writer();
	message.varint(1, _participant).varint(2, _track).message(3, options);
	const auto response = request(Request::kPublishTrack, message);
	_publishAsync = Varint(Sub(Reader(response), Response::kPublishTrack), 1);
	if (!_publishAsync) {
		connectFailed();
	}
}

void Engine::roomEvent(Reader event) {
	// RoomEvent { room_handle = 1, oneof message }.
	auto room = quint64();
	auto copy = event;
	while (copy.next()) {
		if (copy.field() == 1 && !copy.isChunk()) {
			room = copy.varint();
		}
	}
	const auto mine = (room != 0) && (room == _room);
	while (event.next()) {
		if (!event.isChunk()) {
			continue;
		}
		auto body = event.sub();
		switch (event.field()) {
		case Room::kTrackSubscribed: {
			// { participant_identity = 1, track = 2 (OwnedTrack) };
			// TrackInfo { sid = 1, name = 2, kind = 3 }.
			const auto owned = Sub(body, 2);
			auto track = OwnedHandle(owned);
			const auto info = Sub(owned, 2);
			const auto kind = int(Varint(info, 3));
			const auto audio = (kind == kTrackKindAudio);
			if (mine && track && kind == kTrackKindVideo) {
				// The other side's camera. NewVideoStreamRequest {
				// track_handle = 1, type = 2 (native: 0), format = 3,
				// normalize_stride = 4 }: RGBA frames, rows of width * 4.
				closeVideoStream();
				_remoteVideoTrack = track;
				_remoteVideoSid = String(info, 1);
				_remoteVideoMuted = (Varint(info, 5) != 0);
				auto message = Writer();
				message.varint(1, _remoteVideoTrack)
					.varint(2, 0)
					.varint(3, kVideoBufferRgba)
					.boolean(4, true);
				const auto response = request(Request::kNewVideoStream, message);
				_videoStream = OwnedHandle(
					Sub(Sub(Reader(response), Response::kNewVideoStream), 1));
				// Shown with its first frame.
				break;
			}
			if (!mine || !audio || !track) {
				_library->dropHandle(track);
				break;
			}
			closeStream();
			_remoteTrack = track;
			_remoteTrackSid = String(info, 1);
			// NewAudioStreamRequest { track_handle = 1, type = 2 (native: 0),
			// sample_rate = 3, num_channels = 4 }.
			auto message = Writer();
			message.varint(1, _remoteTrack)
				.varint(2, 0)
				.varint(3, Audio::kSampleRate)
				.varint(4, 1);
			const auto response = request(Request::kNewAudioStream, message);
			_stream = OwnedHandle(
				Sub(Sub(Reader(response), Response::kNewAudioStream), 1));
		} break;
		case Room::kTrackUnsubscribed:
			// { participant_identity = 1, track_sid = 2 }.
			if (mine && String(body, 2) == _remoteTrackSid) {
				closeStream();
			} else if (mine
				&& !_remoteVideoSid.isEmpty()
				&& String(body, 2) == _remoteVideoSid) {
				closeVideoStream();
			}
			break;
		case Room::kTrackMuted:
		case Room::kTrackUnmuted:
			// { participant_identity = 1, track_sid = 2 }: a camera that is
			// muted shows nothing (the layout follows the tracks, not the
			// call's flag).
			if (mine
				&& !_remoteVideoSid.isEmpty()
				&& String(body, 2) == _remoteVideoSid) {
				_remoteVideoMuted = (event.field() == Room::kTrackMuted);
				if (_remoteVideoMuted) {
					setRemoteVideoShown(false);
				}
			}
			break;
		case Room::kReconnecting:
			if (mine && _state == Media::State::Connected) {
				setState(Media::State::Reconnecting);
			}
			break;
		case Room::kReconnected:
			if (mine && _state == Media::State::Reconnecting) {
				setState(Media::State::Connected);
			}
			break;
		case Room::kDisconnected:
			if (mine) {
				// The library gave up (or the server closed the room).
				++_generation;
				_connectAsync = 0;
				if (_watchdog) {
					_watchdog->stop();
				}
				closeRoom();
				setState(Media::State::Failed);
			}
			break;
		// Events that hand over handles nobody here needs.
		case Room::kParticipantConnected:
			_library->dropHandle(OwnedHandle(Sub(body, 1)));
			break;
		case Room::kTrackPublished:
			_library->dropHandle(OwnedHandle(Sub(body, 2)));
			break;
		case Room::kDataPacketReceived:
			// { kind = 1, participant_identity = 2, user = 4 { data = 1 (OwnedBuffer) } }.
			_library->dropHandle(OwnedHandle(Sub(Sub(body, 4), 1)));
			break;
		case Room::kByteStreamOpened:
		case Room::kTextStreamOpened:
			_library->dropHandle(OwnedHandle(Sub(body, 1)));
			break;
		}
	}
}

void Engine::audioStreamEvent(Reader event) {
	// AudioStreamEvent { stream_handle = 1, frame_received = 2 { frame = 1
	// (OwnedAudioFrameBuffer { handle = 1, info = 2 }) }, eos = 3 };
	// AudioFrameBufferInfo { data_ptr = 1, num_channels = 2, sample_rate = 3,
	// samples_per_channel = 4 }.
	auto stream = quint64();
	auto frame = Reader();
	while (event.next()) {
		if (event.field() == 1 && !event.isChunk()) {
			stream = event.varint();
		} else if (event.field() == 2 && event.isChunk()) {
			frame = Sub(event.sub(), 1);
		}
	}
	auto handle = quint64();
	auto info = Reader();
	while (frame.next()) {
		if (!frame.isChunk()) {
			continue;
		}
		if (frame.field() == 1) {
			handle = Varint(frame.sub(), 1);
		} else if (frame.field() == 2) {
			info = frame.sub();
		}
	}
	if (!handle) {
		return;
	}
	auto pointer = quint64();
	auto channels = quint64();
	auto rate = quint64();
	auto samples = quint64();
	while (info.next()) {
		switch (info.field()) {
		case 1: pointer = info.varint(); break;
		case 2: channels = info.varint(); break;
		case 3: rate = info.varint(); break;
		case 4: samples = info.varint(); break;
		}
	}
	// The samples belong to the handle: copy before giving it back.
	if (stream == _stream
		&& stream != 0
		&& pointer != 0
		&& channels == 1
		&& rate == quint64(Audio::kSampleRate)
		&& samples > 0
		&& samples <= quint64(Audio::kSampleRate)
		&& _output) {
		_output->push(
			reinterpret_cast<const qint16*>(quintptr(pointer)),
			int(samples));
	}
	_library->dropHandle(handle);
}

void Engine::videoStreamEvent(Reader event) {
	// VideoStreamEvent { stream_handle = 1, frame_received = 2 { buffer = 1
	// (OwnedVideoBuffer { handle = 1, info = 2 }), timestamp_us = 2,
	// rotation = 3 }, eos = 3 }; VideoBufferInfo { type = 1, width = 2,
	// height = 3, data_ptr = 4, stride = 6 }.
	auto stream = quint64();
	auto received = Reader();
	auto have = false;
	while (event.next()) {
		if (event.field() == 1 && !event.isChunk()) {
			stream = event.varint();
		} else if (event.field() == 2 && event.isChunk()) {
			received = event.sub();
			have = true;
		}
	}
	if (!have) {
		return;
	}
	auto handle = quint64();
	auto info = Reader();
	auto rotation = 0;
	while (received.next()) {
		if (received.field() == 1 && received.isChunk()) {
			auto owned = received.sub();
			while (owned.next()) {
				if (!owned.isChunk()) {
					continue;
				}
				if (owned.field() == 1) {
					handle = Varint(owned.sub(), 1);
				} else if (owned.field() == 2) {
					info = owned.sub();
				}
			}
		} else if (received.field() == 3 && !received.isChunk()) {
			rotation = int(received.varint());
		}
	}
	if (!handle) {
		return;
	}
	auto type = quint64(~0ULL);
	auto width = quint64();
	auto height = quint64();
	auto pointer = quint64();
	auto stride = quint64();
	while (info.next()) {
		switch (info.field()) {
		case 1: type = info.varint(); break;
		case 2: width = info.varint(); break;
		case 3: height = info.varint(); break;
		case 4: pointer = info.varint(); break;
		case 6: stride = info.varint(); break;
		}
	}
	const auto now = QDateTime::currentMSecsSinceEpoch();
	// The pixels belong to the handle: copy before giving it back. Frames
	// that come faster than the view repaints are let go.
	if (stream == _videoStream
		&& stream != 0
		&& pointer != 0
		&& type == quint64(kVideoBufferRgba)
		&& width > 0
		&& height > 0
		&& width <= quint64(kMaxRemoteVideoSide)
		&& height <= quint64(kMaxRemoteVideoSide)
		&& (stride == 0 || stride >= width * 4)
		&& !_remoteVideoMuted
		&& now - _remoteVideoFrameMs >= kRemoteVideoMinIntervalMs) {
		_remoteVideoFrameMs = now;
		const auto line = stride ? qsizetype(stride) : qsizetype(width * 4);
		auto frame = QImage(
			reinterpret_cast<const uchar*>(quintptr(pointer)),
			int(width),
			int(height),
			line,
			QImage::Format_RGBA8888).copy();
		if (rotation != 0) {
			// VideoRotation: 1 = 90, 2 = 180, 3 = 270 degrees.
			auto transform = QTransform();
			transform.rotate(90. * rotation);
			frame = frame.transformed(transform);
		}
		if (!frame.isNull()) {
			setRemoteVideoShown(true);
			Q_EMIT remoteVideoFrame(_session, frame);
		}
	}
	_library->dropHandle(handle);
}

void Engine::statsEvent(Reader event) {
	// GetSessionStatsCallback { async_id = 1, error = 2, result = 3 {
	// publisher_stats = 1, subscriber_stats = 2 } }.
	auto asyncId = quint64();
	auto result = Reader();
	while (event.next()) {
		if (event.field() == 1 && !event.isChunk()) {
			asyncId = event.varint();
		} else if (event.field() == 3 && event.isChunk()) {
			result = event.sub();
		}
	}
	if (!asyncId || asyncId != _statsAsync) {
		return;
	}
	_statsAsync = 0;
	auto parsed = SessionStats();
	while (result.next()) {
		if (result.isChunk() && (result.field() == 1 || result.field() == 2)) {
			ParseRtcStats(result.sub(), parsed);
		}
	}
	auto stats = MediaStats();
	if (parsed.have) {
		const auto rtt = (parsed.rtt >= 0.) ? parsed.rtt : parsed.pairRtt;
		stats.valid = true;
		stats.rttMs = std::max(rtt, 0.) * 1000.;
		stats.jitterMs = std::max(parsed.jitter, 0.) * 1000.;
		const auto received = (parsed.received >= _lastReceived)
			? (parsed.received - _lastReceived)
			: parsed.received;
		const auto lost = std::max(
			(parsed.lost >= _lastLost) ? (parsed.lost - _lastLost) : parsed.lost,
			qint64(0));
		_lastReceived = parsed.received;
		_lastLost = parsed.lost;
		const auto expected = double(received) + double(lost);
		stats.lossPercent = (expected > 0.) ? (100. * double(lost) / expected) : 0.;
		stats.mos = EstimateMos(stats.rttMs, stats.jitterMs, stats.lossPercent);
		stats.relayTcp = parsed.localRelayTcp.value(parsed.selectedLocal, false);
	}
	Q_EMIT statsReady(_session, stats);
}

void Engine::captured(qint16 *samples) {
	const auto bytes = quint32(Audio::kFrameSamples * sizeof(qint16));
	if (_apm) {
		// What the loudspeaker played since the last frame, first.
		_output->takeReference(_reference);
		auto done = size_t();
		while (_reference.size() - done >= size_t(Audio::kFrameSamples)) {
			// ApmProcessReverseStreamRequest { apm_handle = 1, data_ptr = 2,
			// size = 3 (bytes), sample_rate = 4, num_channels = 5 }.
			auto message = Writer();
			message.varint(1, _apm)
				.varint(2, quint64(quintptr(_reference.data() + done)))
				.varint(3, bytes)
				.varint(4, Audio::kSampleRate)
				.varint(5, 1);
			[[maybe_unused]] const auto response = request(
				Request::kApmProcessReverseStream,
				message);
			done += Audio::kFrameSamples;
		}
		if (done > 0) {
			_reference.erase(_reference.begin(), _reference.begin() + qsizetype(done));
		}
		// ApmSetStreamDelayRequest { apm_handle = 1, delay_ms = 2 }.
		auto delay = Writer();
		delay.varint(1, _apm)
			.integer(2, _output->delayMs() + _input->delayMs());
		[[maybe_unused]] const auto delayed = request(
			Request::kApmSetStreamDelay,
			delay);
		// ApmProcessStreamRequest: the same fields; changed in place.
		auto message = Writer();
		message.varint(1, _apm)
			.varint(2, quint64(quintptr(samples)))
			.varint(3, bytes)
			.varint(4, Audio::kSampleRate)
			.varint(5, 1);
		[[maybe_unused]] const auto processed = request(
			Request::kApmProcessStream,
			message);
	}
	if (!_muted) {
		_inputLevel = std::max(
			_inputLevel,
			Audio::Level(samples, Audio::kFrameSamples));
		if (_published && _source) {
			// CaptureAudioFrameRequest { source_handle = 1, buffer = 2 };
			// AudioFrameBufferInfo as above. The library copies the samples
			// before it returns.
			auto buffer = Writer();
			buffer.varint(1, quint64(quintptr(samples)))
				.varint(2, 1)
				.varint(3, Audio::kSampleRate)
				.varint(4, Audio::kFrameSamples);
			auto message = Writer();
			message.varint(1, _source).message(2, buffer);
			[[maybe_unused]] const auto response = request(
				Request::kCaptureAudioFrame,
				message);
		}
	}
	if (++_framesSinceLevels >= kLevelsEveryFrames) {
		_framesSinceLevels = 0;
		Q_EMIT levelsChanged(
			_session,
			std::exchange(_inputLevel, 0.f),
			_output->takeLevel());
	}
}

// ---- the process-wide engine and its thread

struct Shared {
	QThread *thread = nullptr;
	Engine *engine = nullptr;
	std::atomic<quint64> nextSession = 1;
	bool stopped = false;
};

[[nodiscard]] Shared &Global() {
	static auto result = Shared();
	return result;
}

[[nodiscard]] Engine *EnsureEngine() {
	auto &shared = Global();
	if (shared.engine || shared.stopped) {
		return shared.engine;
	}
	const auto library = Ffi::Library::Instance();
	if (!library) {
		qWarning(
			"Calls: the media library cannot be used: %s",
			qPrintable(Ffi::Library::LoadError()));
		return nullptr;
	}
	qRegisterMetaType<Calls::MediaStats>();
	shared.thread = new QThread();
	shared.thread->setObjectName(QStringLiteral("Calls media"));
	shared.engine = new Engine(library);
	shared.engine->moveToThread(shared.thread);
	const auto engine = shared.engine;
	library->setEventHandler([engine](QByteArray bytes) {
		QMetaObject::invokeMethod(engine, [=] {
			engine->handleEvent(bytes);
		}, Qt::QueuedConnection);
	});
	shared.thread->start(QThread::HighPriority);
	return shared.engine;
}

template <typename Method>
void Post(Engine *engine, Method &&method) {
	QMetaObject::invokeMethod(
		engine,
		std::forward<Method>(method),
		Qt::QueuedConnection);
}

class LiveKitMedia final : public Media {
public:
	LiveKitMedia(Engine *engine, const MediaSettings &settings, QObject *parent)
	: Media(parent)
	, _engine(engine)
	, _session(Global().nextSession++)
	, _settings(settings) {
		connect(engine, &Engine::stateChanged, this, [this](
				quint64 session,
				int state) {
			if (session == _session && _state != State(state)) {
				_state = State(state);
				Q_EMIT stateChanged(_state);
			}
		});
		connect(engine, &Engine::levelsChanged, this, [this](
				quint64 session,
				float input,
				float output) {
			if (session == _session) {
				Q_EMIT levelsChanged(input, output);
			}
		});
		connect(engine, &Engine::statsReady, this, [this](
				quint64 session,
				const MediaStats &stats) {
			if (session == _session) {
				Q_EMIT statsReady(stats);
			}
		});
		connect(engine, &Engine::inputFailed, this, [this](quint64 session) {
			if (session == _session) {
				Q_EMIT inputFailed();
			}
		});
		connect(engine, &Engine::remoteVideoChanged, this, [this](
				quint64 session,
				bool shown) {
			if (session == _session) {
				Q_EMIT remoteVideoChanged(shown);
			}
		});
		connect(engine, &Engine::remoteVideoFrame, this, [this](
				quint64 session,
				const QImage &frame) {
			if (session == _session) {
				Q_EMIT remoteVideoFrame(frame);
			}
		});
		connect(&_camera, &Video::Camera::frame, this, [this](
				const QImage &frame) {
			if (!_cameraEnabled) {
				return;
			}
			// Implicitly shared: the engine converts its copy on its
			// thread, the view scales this one.
			const auto session = _session;
			Post(_engine, [=, engine = _engine] {
				engine->captureVideo(session, frame);
			});
			Q_EMIT localVideoFrame(frame);
		});
		connect(&_camera, &Video::Camera::failed, this, [this] {
			// Unplugged in the middle of a call: the default one, once.
			if (!_cameraEnabled) {
				return;
			}
			_settings.cameraDeviceId = QByteArray();
			if (!startCamera()) {
				disableCamera();
				Q_EMIT cameraFailed(!Video::HasCamera());
			}
		});
		connect(&_devices, &QMediaDevices::audioInputsChanged, this, [this] {
			devicesChanged(true);
		});
		connect(&_devices, &QMediaDevices::audioOutputsChanged, this, [this] {
			devicesChanged(false);
		});
		_input = pick(true, nullptr);
		_output = pick(false, nullptr);
		const auto session = _session;
		const auto input = _input;
		const auto output = _output;
		Post(_engine, [=, engine = _engine] {
			engine->open(session, settings, input, output);
		});
	}

	~LiveKitMedia() {
		_camera.stop();
		const auto session = _session;
		Post(_engine, [=, engine = _engine] { engine->close(session); });
	}

	void join(const MediaJoin &join) override {
		const auto session = _session;
		// Known at once on this side too: a second join() before the engine
		// answered must not look idle.
		if (_state != State::Connecting && join.valid()) {
			_state = State::Connecting;
			Q_EMIT stateChanged(_state);
		}
		Post(_engine, [=, engine = _engine] { engine->join(session, join); });
	}

	void leave() override {
		const auto session = _session;
		// The call is over for this side: the camera's light goes off.
		disableCamera();
		if (_state != State::Idle) {
			_state = State::Idle;
			Q_EMIT stateChanged(_state);
		}
		Post(_engine, [=, engine = _engine] { engine->leave(session); });
	}

	State state() const override {
		return _state;
	}

	void setMuted(bool muted) override {
		const auto session = _session;
		Post(_engine, [=, engine = _engine] {
			engine->setMuted(session, muted);
		});
	}

	void setInputDevice(const QByteArray &id) override {
		_settings.inputDeviceId = id;
		_input = pick(true, nullptr);
		sendDevice(true);
	}

	void setOutputDevice(const QByteArray &id) override {
		_settings.outputDeviceId = id;
		_output = pick(false, nullptr);
		sendDevice(false);
	}

	void setProcessing(bool echo, bool noise, bool gain) override {
		const auto session = _session;
		Post(_engine, [=, engine = _engine] {
			engine->setProcessing(session, echo, noise, gain);
		});
	}

	void requestStats() override {
		const auto session = _session;
		Post(_engine, [=, engine = _engine] { engine->requestStats(session); });
	}

	void setCameraEnabled(bool enabled) override {
		if (_cameraEnabled == enabled) {
			return;
		}
		if (!enabled) {
			disableCamera();
			return;
		}
		if (!startCamera()) {
			Q_EMIT cameraFailed(!Video::HasCamera());
			return;
		}
		_cameraEnabled = true;
		const auto session = _session;
		Post(_engine, [=, engine = _engine] {
			engine->setCamera(session, true);
		});
		Q_EMIT cameraChanged(true);
	}

	bool cameraEnabled() const override {
		return _cameraEnabled;
	}

	void setCameraDevice(const QByteArray &id) override {
		_settings.cameraDeviceId = id;
		if (_cameraEnabled && !startCamera()) {
			disableCamera();
			Q_EMIT cameraFailed(!Video::HasCamera());
		}
	}

private:
	[[nodiscard]] bool startCamera() {
#ifdef ELEZA_DEBUG_TOOLS
		if (!_settings.debugCamera.isEmpty()) {
			_camera.startPattern();
			return true;
		}
#endif // ELEZA_DEBUG_TOOLS
		return _camera.start(_settings.cameraDeviceId);
	}

	void disableCamera() {
		_camera.stop();
		if (!_cameraEnabled) {
			return;
		}
		_cameraEnabled = false;
		const auto session = _session;
		Post(_engine, [=, engine = _engine] {
			engine->setCamera(session, false);
		});
		Q_EMIT cameraChanged(false);
	}

	[[nodiscard]] QAudioDevice pick(bool input, bool *found) const {
		return input
			? Audio::PickDevice(
				QMediaDevices::audioInputs(),
				QMediaDevices::defaultAudioInput(),
				_settings.inputDeviceId,
				found)
			: Audio::PickDevice(
				QMediaDevices::audioOutputs(),
				QMediaDevices::defaultAudioOutput(),
				_settings.outputDeviceId,
				found);
	}

	void sendDevice(bool input) {
		const auto session = _session;
		const auto device = input ? _input : _output;
		Post(_engine, [=, engine = _engine] {
			if (input) {
				engine->setInput(session, device);
			} else {
				engine->setOutput(session, device);
			}
		});
	}

	// A device appeared or went away, or the system's default changed.
	void devicesChanged(bool input) {
		auto found = true;
		const auto now = pick(input, &found);
		auto &current = input ? _input : _output;
		auto &chosen = input ? _settings.inputDeviceId : _settings.outputDeviceId;
		if (!found) {
			// The chosen one was unplugged: the default from here on.
			chosen = QByteArray();
			Q_EMIT deviceFallback(input);
		}
		if (now.id() != current.id() || now.isNull() != current.isNull()) {
			current = now;
			sendDevice(input);
		}
	}

	Engine *_engine = nullptr;
	quint64 _session = 0;
	MediaSettings _settings;
	QMediaDevices _devices;
	QAudioDevice _input;
	QAudioDevice _output;
	Video::Camera _camera;
	bool _cameraEnabled = false;
	State _state = State::Idle;

};

} // namespace

std::unique_ptr<Media> CreateMedia(
		const MediaSettings &settings,
		QObject *parent) {
	const auto engine = EnsureEngine();
	if (!engine) {
		return nullptr;
	}
	return std::make_unique<LiveKitMedia>(engine, settings, parent);
}

void ShutdownMedia(int timeoutMs) {
	auto &shared = Global();
	shared.stopped = true;
	const auto engine = std::exchange(shared.engine, nullptr);
	const auto thread = std::exchange(shared.thread, nullptr);
	if (!engine || !thread) {
		return;
	}
	// The engine leaves the room and stops the library on its own thread;
	// this one waits a bounded time and then lets the process end anyway.
	auto done = std::make_shared<QSemaphore>();
	QMetaObject::invokeMethod(engine, [=] {
		engine->shutdown();
		done->release();
	}, Qt::QueuedConnection);
	if (done->tryAcquire(1, timeoutMs)) {
		thread->quit();
		if (thread->wait(500)) {
			delete engine;
			delete thread;
		}
	}
	// Otherwise the thread is left as it is: the process is exiting.
}

} // namespace Calls

#include "calls_media_livekit.moc"
