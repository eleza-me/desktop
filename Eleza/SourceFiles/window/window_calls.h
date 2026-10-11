// Voice calls as the rest of the app sees them: the chat header, the profile
// and a call record in the history start a call through here and never
// include anything of the calls module (calls/), which registers itself at
// startup - or does not exist in a build without calls (ELEZA_ENABLE_CALLS=OFF),
// in which case nothing shows a call button.
//
//   if (Window::CallsSupported()) { … show the button … }
//   Window::StartCall(controller, userId);   // starts it, or brings up the call with them
//   if (Window::VideoCallsOffered(controller)) { … the video call button … }
//   Window::StartCall(controller, userId, true);   // … as a video call
//
// Whether a chat may have calls at all is Data::CallAvailabilityFor(chat).
#pragma once

#include <QtCore/QString>

#include <functional>

namespace Window {

class SessionController;

struct CallHooks {
	// Calls the person; when a call with them is going on, shows it.
	// `video`: as a video call.
	std::function<void(
		SessionController *controller,
		const QString &userId,
		bool video)> start;
};

// calls/calls_module.cpp.
void RegisterCallHooks(CallHooks hooks);

[[nodiscard]] bool CallsSupported();
void StartCall(
	SessionController *controller,
	const QString &userId,
	bool video = false);
// A build with calls, on a server that lists `feature.video_calls`: absent
// or false means no camera control anywhere and no `video` in a request.
[[nodiscard]] bool VideoCallsOffered(SessionController *controller);

} // namespace Window
