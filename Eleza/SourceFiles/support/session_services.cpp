#include "support/session_services.h"

#include "support/error_text.h"
#include "support/notifications.h"
#include "window/window_session_controller.h"

namespace Support {

SessionServices *SessionServices::For(Window::SessionController *controller) {
	if (!controller) {
		return nullptr;
	}
	const auto existing = controller->findChild<SessionServices*>(
		QString(),
		Qt::FindDirectChildrenOnly);
	return existing ? existing : new SessionServices(controller);
}

SessionServices::SessionServices(Window::SessionController *controller)
: QObject(controller)
, _errors(new ErrorReporter(controller, this))
, _notifications(new NotificationsManager(controller, this)) {
}

} // namespace Support
