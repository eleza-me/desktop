#include "media/media_module.h"

#include "media/media_shared.h"
#include "window/window_registry.h"
#include "window/window_section_widget.h"
#include "window/window_session_controller.h"

namespace Media {

void Register() {
	// chat/<id>?show=media; any other chat route goes on to the chat screen
	// (this handler is registered before it).
	Window::RegisterRouteHandler(Window::RouteType::Chat, [](
			Window::SessionController *controller,
			const Window::Route &route) {
		if (route.id.isEmpty()
			|| route.queryValue(QStringLiteral("show")) != QStringLiteral("media")) {
			return false;
		}
		// Over whatever opened it: the chat, the profile.
		controller->showMainSection(SharedMediaFactory(route.id));
		return true;
	});
}

} // namespace Media
