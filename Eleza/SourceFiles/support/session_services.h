// What every signed-in window needs besides its sections: the error reporter
// (toasts for failed background actions) and desktop notifications. Created by
// Core::Application right after the session layout, destroyed with it.
//
//   Support::SessionServices::For(controller)->notifications()
#pragma once

#include <QtCore/QObject>

namespace Window {
class SessionController;
} // namespace Window

namespace Support {

class ErrorReporter;
class NotificationsManager;

class SessionServices final : public QObject {
	Q_OBJECT

public:
	// Creates them on first use; owned by the controller.
	[[nodiscard]] static SessionServices *For(
		Window::SessionController *controller);

	[[nodiscard]] NotificationsManager *notifications() const {
		return _notifications;
	}

private:
	explicit SessionServices(Window::SessionController *controller);

	ErrorReporter *_errors = nullptr;
	NotificationsManager *_notifications = nullptr;

};

// Registers the "debug/…" routes of the shared helpers (a sample notification,
// the image viewer, the crop box, …). Called from Core::RegisterModules.
#ifdef ELEZA_DEBUG_TOOLS
void RegisterDebugRoutes();
#endif // ELEZA_DEBUG_TOOLS

} // namespace Support
