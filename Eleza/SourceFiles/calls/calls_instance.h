// The calls of one signed-in session: at most one at a time.
//
//   instance->start(userId)          I call somebody
//   signal call.incoming             somebody calls me -> a ringing Call
//   instance->call()                 the call there is (live, or just ended and
//                                    still shown), or null
//
// Besides owning the call it keeps the rules that are about "which call":
//
//  * One call per device. A call.incoming while a call is live is not shown:
//    the server gave the caller "busy" already when I am really in a call.
//    The one exception is two people calling each other at the same moment:
//    my start is then refused as busy while theirs rings here - the incoming
//    call that arrived meanwhile is shown as soon as mine has failed.
//  * A call that rang while this device could not hear the signal (offline,
//    the app not running) is asked for once the realtime connection is up,
//    and once a minute while it is down; never more often than once a minute.
//  * The call this device is in is remembered across a restart of the app:
//    after one, an answered call is joined again (GET /v1/calls/active says
//    whether it still exists), a call that was only ringing there is
//    cancelled. A call the user is in on another device is left alone.
//  * A video call needs nothing more than a voice call: without a camera (or
//    with one that cannot be opened) it goes on with audio and a notice.
//  * No microphone, no call: starting and answering are refused with a
//    notice instead of connecting a call the other side cannot hear.
//
// No widgets (tests/app/test_calls_call.cpp); the window's side of it is
// calls_module.cpp.
#pragma once

#include "calls/calls_call.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>

namespace Calls {

enum class Notice {
	NoMicrophone,      // there is no audio input on this computer
	MediaUnavailable,  // the media library is missing or broken
	AlreadyInCall,     // start() while another call is live
	AnsweredElsewhere,
	ReconnectFailed,
	MicrophoneFailed,  // the microphone stopped working during the call
	InputDeviceChanged,  // the chosen microphone is gone: the default is used
	OutputDeviceChanged,
	NoCamera,          // the camera was asked for and there is none
	CameraFailed,      // … or it could not be opened: the call goes on with audio
};

class Instance final : public QObject {
	Q_OBJECT

public:
	struct Args {
		Data::CallsApi *api = nullptr;
		Fn<std::unique_ptr<Media>()> createMedia;
		// Why no media can be created (MediaProblem::None: it can).
		Fn<MediaProblem()> checkMedia;
		Fn<bool()> hasMicrophone;
		Fn<QString()> networkType;
		// The id of the call this device is in, kept across restarts.
		Fn<QString()> loadStoredCall;
		Fn<void(const QString &)> storeCall;
		Timings timings;
		int incomingCheckIntervalMs = 60000;
	};

	explicit Instance(Args args, QObject *parent = nullptr);
	~Instance();

	// The session's cached state is loaded: recover a call from before a
	// restart, ask whether something rings.
	void sessionReady();

	// Null when there is none. An ended call stays until dismissEnded()
	// (it is on screen for a moment) or until the next call.
	[[nodiscard]] Call *call() const {
		return _call.get();
	}
	// A call is being set up, ringing or going on.
	[[nodiscard]] bool live() const {
		return _call && !_call->ended();
	}
	// True when a call to that person is now there (started, or was live).
	// `video`: as a video call (my camera starts with it).
	bool start(const QString &peerId, bool video = false);
	// Accepts the call that rings here; `withCamera`: with my camera on.
	void answer(bool withCamera = false);
	void dismissEnded();

	void handleSignal(const Data::CallSignal &signal);
	void signalsConnectedChanged(bool connected);
	// The app is exiting: ends the call for the server; `done` is called
	// when that is out (or at once).
	void finishOnExit(Fn<void()> done);

Q_SIGNALS:
	// call() is another call now, or none.
	void callChanged();
	// The state, the mute flag, the id, the video intent or one of the
	// cameras of call() changed.
	void callUpdated();
	// call() ended (still there: read endReason(), quiet(), startError()).
	void callEnded();
	void notice(Calls::Notice notice);

private:
	[[nodiscard]] Call::Args callArgs(
		const QString &peerId,
		bool video = false) const;
	void setCall(std::unique_ptr<Call> call);
	void watch(Call *call);
	void ring(const QString &call, const QString &from, bool video);
	void callStateChanged(Call *call);
	void recover();
	void checkIncoming(bool force);
	void updateIncomingTimer();

	Args _args;
	std::unique_ptr<Call> _call;
	// Two people called each other: theirs waits for mine to fail.
	std::optional<Data::CallSignal> _waitingIncoming;
	// The id of the call that ended last: it never rings again.
	QString _endedCall;
	base::Timer _incomingTimer;
	QElapsedTimer _lastIncomingCheck;
	bool _incomingCheckInFlight = false;
	bool _signalsConnected = false;
	bool _ready = false;

};

} // namespace Calls

Q_DECLARE_METATYPE(Calls::Notice)
