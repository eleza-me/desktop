#include "window/window_calls.h"

#include "api/api_client_config.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"

namespace Window {
namespace {

[[nodiscard]] CallHooks &Hooks() {
	static auto result = CallHooks();
	return result;
}

} // namespace

void RegisterCallHooks(CallHooks hooks) {
	Hooks() = std::move(hooks);
}

bool CallsSupported() {
	return Hooks().start != nullptr;
}

void StartCall(
		SessionController *controller,
		const QString &userId,
		bool video) {
	if (controller && !userId.isEmpty() && Hooks().start) {
		Hooks().start(
			controller,
			userId,
			video && VideoCallsOffered(controller));
	}
}

bool VideoCallsOffered(SessionController *controller) {
	return controller
		&& CallsSupported()
		&& controller->session().account().clientConfig().videoCallsEnabled();
}

} // namespace Window
