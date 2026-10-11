#include "window/window_calls.h"

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

void StartCall(SessionController *controller, const QString &userId) {
	if (controller && !userId.isEmpty() && Hooks().start) {
		Hooks().start(controller, userId);
	}
}

} // namespace Window
