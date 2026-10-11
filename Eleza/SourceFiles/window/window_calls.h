// Voice calls as the rest of the app sees them: the chat header, the profile
// and a call record in the history start a call through here and never
// include anything of the calls module (calls/), which registers itself at
// startup - or does not exist in a build without calls (ELEZA_ENABLE_CALLS=OFF),
// in which case nothing shows a call button.
//
//   if (Window::CallsSupported()) { … show the button … }
//   Window::StartCall(controller, userId);   // starts it, or brings up the call with them
//
// Whether a chat may have calls at all is Data::CallAvailabilityFor(chat).
#pragma once

#include <QtCore/QString>

#include <functional>

namespace Window {

class SessionController;

struct CallHooks {
	// Calls the person; when a call with them is going on, shows it.
	std::function<void(
		SessionController *controller,
		const QString &userId)> start;
};

// calls/calls_module.cpp.
void RegisterCallHooks(CallHooks hooks);

[[nodiscard]] bool CallsSupported();
void StartCall(SessionController *controller, const QString &userId);

} // namespace Window
