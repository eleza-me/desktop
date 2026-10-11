// The audio of a call, behind an interface: the rest of the app never sees
// the media library.
//
// Every call is a room of two on the SFU the server names (the join payload:
// sfu_url, token, turn { urls, username, credential }, relay_only). The media
// connects there, publishes the microphone, plays the other side and reports
// what the connection does:
//
//   auto media = Calls::CreateMedia(settings);      // null: Calls::CheckMedia() says why
//   connect(media.get(), &Calls::Media::stateChanged, ...);
//   media->join(Calls::MediaJoin::FromJson(join));   // again after /rejoin: fresh credentials
//   media->setMuted(true);
//   media->leave();
//
// States: Idle -> Connecting -> Connected <-> Reconnecting -> Failed. Failed
// means the connection is gone and the library gave up: the call asks the
// server for fresh credentials (rejoin) and joins again, or ends.
//
// Nothing here blocks: the implementation works on a thread of its own and
// reports through signals on the thread of the object. Only audio is
// carried; a video track would be one more pair of source / stream next to
// the audio ones in calls_media_livekit.cpp.
#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QStringList>

#include <memory>

namespace Calls {

struct MediaJoin {
	QString url;
	QString token;
	QStringList turnUrls;
	QString turnUsername;
	QString turnCredential;
	// The server sets it on every call: no path between the two users ever
	// exists, the only remote end is the SFU (directly, or through the TURN
	// servers of the join when the network allows nothing else).
	bool relayOnly = true;

	[[nodiscard]] bool valid() const {
		return !url.isEmpty() && !token.isEmpty();
	}
	// The join payload of POST /v1/calls, …/accept and …/rejoin.
	[[nodiscard]] static MediaJoin FromJson(const QJsonObject &join);
};

// What the user chose for calls (Core::Settings); device ids are those of
// QAudioDevice::id(), empty = the default device of the system.
struct MediaSettings {
	QByteArray inputDeviceId;
	QByteArray outputDeviceId;
	bool echoCancellation = true;
	bool noiseSuppression = true;
	bool autoGainControl = true;
#ifdef ELEZA_DEBUG_TOOLS
	// Audio without sound hardware (tests/tools/calls-e2e.sh):
	//   debugInput   "tone:<Hz>" - a generated signal instead of the microphone
	//   debugOutput  a .wav file that records what would be played
	QString debugInput;
	QString debugOutput;
#endif // ELEZA_DEBUG_TOOLS
};

// A summary for POST /v1/calls/{id}/qos (no content, numbers only).
struct MediaStats {
	bool valid = false;
	double rttMs = 0.;
	double jitterMs = 0.;
	double lossPercent = 0.;
	double mos = 0.;
	bool relayTcp = false;
};

// MOS estimate (1 … 4.5) from round trip, jitter and loss: the simplified
// E-model the web clients of the SFU use.
[[nodiscard]] double EstimateMos(double rttMs, double jitterMs, double lossPercent);

enum class MediaProblem {
	None,
	LibraryMissing, // the media library is not next to the app
	LibraryBroken,  // it is there but cannot be loaded
};
// Whether calls can work in this copy of the app (cheap: looks for the file;
// the library is loaded when the first call needs it).
[[nodiscard]] MediaProblem CheckMedia();

class Media : public QObject {
	Q_OBJECT

public:
	enum class State {
		Idle,
		Connecting,
		Connected,
		Reconnecting,
		Failed,
	};
	Q_ENUM(State)

	using QObject::QObject;

	// Connects to the room (again, with new credentials, when called while
	// connected or after Failed).
	virtual void join(const MediaJoin &join) = 0;
	// Leaves the room; the object can join again.
	virtual void leave() = 0;
	[[nodiscard]] virtual State state() const = 0;
	[[nodiscard]] bool isConnected() const {
		return state() == State::Connected;
	}

	virtual void setMuted(bool muted) = 0;
	// Device ids of QAudioDevice; empty = the system's default.
	virtual void setInputDevice(const QByteArray &id) = 0;
	virtual void setOutputDevice(const QByteArray &id) = 0;
	virtual void setProcessing(
		bool echoCancellation,
		bool noiseSuppression,
		bool autoGainControl) = 0;
	// statsReady() follows (with valid = false when there is nothing yet).
	virtual void requestStats() = 0;

Q_SIGNALS:
	void stateChanged(Calls::Media::State state);
	// 0..1, a few times per second while connected: the microphone after
	// processing (0 while muted) and the other side.
	void levelsChanged(float input, float output);
	void statsReady(const Calls::MediaStats &stats);
	// The chosen device is gone (unplugged): the default one is used now.
	void deviceFallback(bool input);
	// No microphone can be opened (none, or the system refuses): the call
	// goes on, the other side hears nothing.
	void inputFailed();

};

// Null when CheckMedia() reports a problem (or the library fails to load).
[[nodiscard]] std::unique_ptr<Media> CreateMedia(
	const MediaSettings &settings,
	QObject *parent = nullptr);

// Leaves every room and stops the media thread; waits for it at most
// `timeoutMs`. Called once, when the app exits.
void ShutdownMedia(int timeoutMs = 1500);

} // namespace Calls

Q_DECLARE_METATYPE(Calls::MediaStats)
