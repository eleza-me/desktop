// One voice call from this device's point of view: the state machine between
// the server's commands and signals (data/data_calls.h), the audio connection
// (calls/calls_media.h) and whatever shows the call.
//
//   Calling       I started it; the server has not said yet that it rings there
//   Ringing       it rings on their device
//   Incoming      it rings here
//   Connecting    answered (by either side); the audio is on its way
//   InCall        both talk
//   Reconnecting  the audio connection dropped; trying to get it back
//   Ended         over (endReason() says how; quiet() = nothing to show)
//
// The behaviour follows the mobile apps, so that the three clients agree on
// who tells the server what:
//
//  * The caller joins the audio room as soon as the call exists, the callee
//    when it answers; "connected" is reported to the server once this side's
//    audio is up and the call was answered.
//  * End: reject while it rings here, cancel while it rings there, hangup
//    after the answer - told once, whichever path gets there first. End
//    pressed before the server answered the start cancels the call as soon as
//    its id is known.
//  * The call's state is polled while it is up (the server's heartbeat rule):
//    every 10 s in the call, every 5 s while ringing / connecting with the
//    realtime connection up, every 2 s without it.
//  * A dropped audio connection gets fresh credentials (rejoin) and joins
//    again; everything that retries shares one budget of 30 s, then the call
//    is ended for both sides.
//  * It rings here for at most 60 s whatever the server says.
//
// Video. A call has a `video` intent, set by whoever starts it and repeated
// by the server wherever a side learns about the call; it decides how the
// call is worded and whether my camera starts with it. The camera itself is
// a track: setCameraEnabled() turns mine on or off at any time in a call of
// either kind, remoteVideo() tells whether theirs is to be shown. A camera
// that cannot be opened, and a server that has video calls switched off
// (`call.video_disabled`: the call is started again as a voice call), leave
// a call with audio.
//
// No widgets: tests/app/test_calls_call.cpp drives it with a fake server and a
// fake media.
#pragma once

#include "base/timer.h"
#include "calls/calls_media.h"
#include "data/data_calls.h"

#include <QtCore/QElapsedTimer>

#include <memory>

namespace Calls {

enum class State {
	Calling,
	Ringing,
	Incoming,
	Connecting,
	InCall,
	Reconnecting,
	Ended,
};

enum class EndReason {
	None,
	Hangup,            // ended normally, by either side
	Declined,          // they declined my call
	NoAnswer,          // nobody answered my call
	Busy,              // they are in another call
	NotAllowed,        // the server refuses calls to this person
	Failed,            // could not be started or connected
	Lost,              // the connection was lost
	// Ended while it was ringing here: nothing to show.
	DeclinedByMe,
	Missed,            // the caller gave up or it rang out
	AnsweredElsewhere, // another device of mine answered
};

// The server's end_reason of a call I was in or had started.
[[nodiscard]] EndReason EndReasonFromServer(const QString &reason);
// The error of POST /v1/calls as an end reason.
[[nodiscard]] EndReason EndReasonFromStartError(const Api::Error &error);

struct Timings {
	int pollInCallMs = 10000;
	int pollConnectedMs = 5000;
	int pollOfflineMs = 2000;
	int ringTimeoutMs = 60000;
	int reconnectBudgetMs = 30000;
	int rejoinDelayMs = 2000; // doubled twice: 2 s, 4 s, 8 s, 8 s…
	int qosFirstMs = 15000;
	int qosEachMs = 150000;
};

class Call final : public QObject {
	Q_OBJECT

public:
	struct Args {
		Data::CallsApi *api = nullptr;
		// Null result: no audio on this machine (the library is missing).
		Fn<std::unique_ptr<Media>()> createMedia;
		// "wifi" | "ethernet" | "cellular" | "unknown" for the quality reports.
		Fn<QString()> networkType;
		QString peerId;
		// I start it as a video call (start() only; a call that rings here
		// or is resumed says so itself).
		bool video = false;
		Timings timings;
	};

	// The call is not started yet: start() / ring() / resume() does it.
	explicit Call(Args args, QObject *parent = nullptr);
	~Call();

	// I call args.peerId.
	void start();
	// `call` rings here.
	// (`video`: it was started as a video call.)
	void ring(const QString &call, bool video = false);
	// A call this device was in before the app restarted (answered already).
	void resume(const Data::ActiveCall &call);

	[[nodiscard]] State state() const {
		return _state;
	}
	[[nodiscard]] bool ended() const {
		return _state == State::Ended;
	}
	[[nodiscard]] EndReason endReason() const {
		return _endReason;
	}
	// Ended without ever being a call on this device (it only rang here, or
	// End was pressed before it existed).
	[[nodiscard]] bool quiet() const {
		return _quiet;
	}
	[[nodiscard]] QString id() const {
		return _id;
	}
	[[nodiscard]] QString peerId() const {
		return _args.peerId;
	}
	[[nodiscard]] bool outgoing() const {
		return _outgoing;
	}
	[[nodiscard]] bool muted() const {
		return _muted;
	}
	// The call was started as a video call.
	[[nodiscard]] bool video() const {
		return _video;
	}
	// My camera is on (or will be as soon as the audio room is joined).
	[[nodiscard]] bool cameraEnabled() const {
		return _cameraWanted;
	}
	// The other side shows a camera right now.
	[[nodiscard]] bool remoteVideo() const {
		return _remoteVideo;
	}
	// When both sides started talking (ms since the epoch; 0 before).
	[[nodiscard]] qint64 connectedAt() const {
		return _connectedAt;
	}
	// Of an ended call that was connected, in seconds; -1 otherwise.
	[[nodiscard]] qint64 duration() const;
	// What the server said when it refused to start the call.
	[[nodiscard]] const std::optional<Api::Error> &startError() const {
		return _startError;
	}
	// Null until the audio room is joined for the first time.
	[[nodiscard]] Media *media() const {
		return _media.get();
	}

	// Accept (it rings here).
	// `withCamera`: also turn my camera on (offered for a video call;
	// «فقط صدا» answers without it).
	void answer(bool withCamera = false);
	// End / decline / cancel.
	void hangup();
	void setMuted(bool muted);
	// Never fails here: cameraChanged(false) and cameraFailed() follow when
	// the camera could not be opened.
	void setCameraEnabled(bool enabled);
	void setCameraDevice(const QByteArray &id);

	// From the session's realtime connection.
	void handleSignal(const Data::CallSignal &signal);
	void signalsConnectedChanged(bool connected);
	// The app is exiting: tells the server (when there is something to
	// tell) and calls `done` when the request is answered - or at once.
	void finishOnExit(Fn<void()> done);

#ifdef ELEZA_DEBUG_TOOLS
	// Screenshots of a call that "has been going on for a while".
	void debugSetConnectedAt(qint64 ms) {
		_connectedAt = ms;
	}
#endif // ELEZA_DEBUG_TOOLS

Q_SIGNALS:
	void stateChanged(Calls::State state);
	void mutedChanged(bool muted);
	// The server knows the call now (its id exists).
	void idAssigned(const QString &id);
	// The audio could not be brought back in time and the call was ended.
	void reconnectFailed();
	// From the media of the call (calls_media.h).
	void levelsChanged(float input, float output);
	void deviceFallback(bool input);
	void inputFailed();
	// The intent became known or changed (a video call the server would not
	// start was started again as a voice call).
	void videoChanged(bool video);
	void cameraChanged(bool enabled);
	void cameraFailed(bool noDevice);
	void remoteVideoChanged(bool shown);
	void localVideoFrame(const QImage &frame);
	void remoteVideoFrame(const QImage &frame);

private:
	// Shared with the callbacks of the start request, which outlive the
	// object when End is pressed (or the app exits) before the answer.
	struct PendingStart {
		bool answered = false;
		bool abandoned = false;
		Fn<void()> afterCancel;
	};

	void setState(State state);
	void end(EndReason reason, bool quiet = false);
	void started(const Data::CallJoin &join);
	void startFailed(const Api::Error &error);
	void sendStart();
	void setVideo(bool video);
	void applyServerState(
		const QString &state,
		const QString &reason,
		qint64 durationS);
	void joinMedia(const Data::CallJoin &join);
	void mediaStateChanged(Media::State state);
	void mediaConnected();
	void reportConnected();
	void poll();
	void schedulePoll();
	void rejoin();
	void startReconnectClock();
	void stopReconnectClock();
	void failCall();
	void tellServer(State was, Fn<void()> done = nullptr);
	[[nodiscard]] bool shouldTellOnExit() const;
	void scheduleQos(int delayMs);
	void sendQos(const MediaStats &stats);

	Args _args;
	std::unique_ptr<Media> _media;
	State _state = State::Calling;
	EndReason _endReason = EndReason::None;
	std::optional<Api::Error> _startError;
	QString _id;
	bool _started = false;
	bool _outgoing = false;
	bool _quiet = false;
	bool _muted = false;
	bool _video = false;
	bool _cameraWanted = false;
	bool _remoteVideo = false;
	bool _videoFallbackTried = false;
	QByteArray _cameraDevice;
	bool _cameraDeviceSet = false;
	// The call was answered (by them, or by me on this device).
	bool _accepted = false;
	// The server already knows this side is done.
	bool _told = false;
	bool _connectedReported = false;
	bool _signalsConnected = false;
	bool _pollInFlight = false;
	qint64 _connectedAt = 0;
	qint64 _endedDuration = -1;
	// Signals of a call whose id this side does not know yet.
	QVector<Data::CallSignal> _earlySignals;

	base::Timer _pollTimer;
	base::Timer _ringTimer;
	base::Timer _rejoinTimer;
	base::Timer _reconnectWatchdog;
	base::Timer _qosTimer;
	QElapsedTimer _reconnectClock; // valid = the audio connection is lost
	int _rejoinTries = 0;
	bool _rejoinInFlight = false;
	std::shared_ptr<PendingStart> _pendingStart;

};

} // namespace Calls
