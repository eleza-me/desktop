// Voice calls of the signed-in session, with everything that is not a part of
// one window: created when the session's layout first appears, gone with the
// session. The window's own pieces (the bar and the expanded view,
// calls_panel.h) come and go with the layout - a language switch rebuilds
// them in the middle of a call - and read everything from here.
//
//   Calls::Service::For(&session)->start(userId);
//
// It owns
//   * the Calls::Instance (which call there is) and feeds it the session's
//     signals;
//   * the sounds: ringtone, ringback, the tones of connecting and ending;
//   * the incoming call window, which must work with the main window
//     minimized or in the tray;
//   * what the user chose for calls (Core::Settings::calls()) on its way to
//     the audio of the running call;
//   * the last picture of each camera of a video call, for whatever shows
//     the call.
#pragma once

#include "calls/calls_instance.h"

#include <QtCore/QPointer>

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Calls {

class Host;
class IncomingWindow;
class Sounds;

class Service final : public QObject {
	Q_OBJECT

public:
	explicit Service(Main::Session *session);
	~Service();

	// Null when the session has none (yet).
	[[nodiscard]] static Service *For(Main::Session *session);
	[[nodiscard]] static Service *Current();

	[[nodiscard]] Main::Session *session() const {
		return _session;
	}
	[[nodiscard]] Instance *instance() const {
		return _instance.get();
	}
	// The call there is (live, or ended and still shown), or null.
	[[nodiscard]] Call *call() const {
		return _instance ? _instance->call() : nullptr;
	}
	// A call is being set up, ringing or going on.
	[[nodiscard]] bool live() const {
		return _instance && _instance->live();
	}
	// A call this device takes part in (not one that only rings here).
	[[nodiscard]] bool engaged() const;

	// The layout that shows the session now (null: none).
	void setController(Window::SessionController *controller);
	[[nodiscard]] Window::SessionController *controller() const;

	// `video`: as a video call (my camera starts with it).
	void start(const QString &userId, bool video = false);
	// `withCamera`: «تصویری» of a video call.
	void answer(bool withCamera = false);
	void hangup();
	void callAgain();
	void toggleMuted();
	void dismissEnded();

	// ---- video
	// The server has video calls (`feature.video_calls`): the camera
	// controls exist.
	[[nodiscard]] bool videoOffered() const;
	void toggleCamera();
	// A device id of Calls::Video::Cameras() (empty: the system's default):
	// saved in the settings and applied to the running call.
	void setCameraDevice(const QByteArray &id);
	// The last frame of each side (null: nothing to show).
	[[nodiscard]] const QImage &localFrame() const {
		return _localFrame;
	}
	[[nodiscard]] const QImage &remoteFrame() const {
		return _remoteFrame;
	}
	// Device ids of QAudioDevice (empty: the system's default): saved in
	// the settings and applied to the running call.
	void setInputDevice(const QByteArray &id);
	void setOutputDevice(const QByteArray &id);

	// The expanded call view over the window instead of the bar.
	[[nodiscard]] bool expanded() const {
		return _expanded;
	}
	void setExpanded(bool expanded);
	// Brings the main window (with the call) to the front.
	void showWindow();

	// 0..1, lately: my microphone, the other side.
	[[nodiscard]] float inputLevel() const {
		return _inputLevel;
	}
	[[nodiscard]] float outputLevel() const {
		return _outputLevel;
	}

	[[nodiscard]] Sounds *sounds() const {
		return _sounds.get();
	}

	void finishOnExit(Fn<void()> done);

#ifdef ELEZA_DEBUG_TOOLS
	// "debug/calls/<what>": a call in a given state without a server, for
	// screenshots (calls_debug.cpp). False for an unknown state.
	bool showDemo(const QString &what);
	void replaceInstance(std::unique_ptr<Instance> instance);
	// ELEZA_DEBUG_CALL_CONTROL=<folder>: commands from a file and the state
	// of the call into another one, for tests/tools/calls-e2e.sh.
	void startDebugControl();
#endif // ELEZA_DEBUG_TOOLS

Q_SIGNALS:
	// call() is another one (or none).
	void callChanged();
	// Its state, mute flag or end changed.
	void callUpdated();
	void expandedChanged();
	void levelsChanged();
	// A new picture of one of the cameras (or one of them went away).
	void videoFrameChanged();
	void noticeShown(Calls::Notice notice);

private:
	[[nodiscard]] Instance::Args instanceArgs();
	[[nodiscard]] MediaSettings mediaSettings() const;
	void adopt(std::unique_ptr<Instance> instance);
	void watchCall();
	void refresh();
	void callEnded();
	void showNotice(Notice notice);
	void settingsChanged();
	void updateIncomingWindow();
	void dropIncomingWindow();
	void flashTaskbar();

	Main::Session *_session = nullptr;
	QPointer<Window::SessionController> _controller;
	QPointer<Host> _host;
	std::unique_ptr<Sounds> _sounds;
	std::unique_ptr<IncomingWindow> _incoming;
	std::unique_ptr<Instance> _instance;
	QPointer<Call> _watched;
	base::Timer _dismissTimer;
	State _lastState = State::Ended;
	bool _wasInCall = false;
	bool _expanded = false;
	float _inputLevel = 0.;
	float _outputLevel = 0.;
	QImage _localFrame;
	QImage _remoteFrame;
	bool _echoCancellation = true;
	bool _noiseSuppression = true;

};

} // namespace Calls
