#include "core/core_modules.h"

#include "intro/intro_module.h"
#include "support/session_services.h"

// Feature modules: include the header that declares `Register()` and call it below.
// (Until a tab has a module it shows a placeholder, and so does a route nobody
// registered a handler for: see window/window_registry.h.)
//
//   #include "dialogs/dialogs_module.h"

namespace Core {

void RegisterModules() {
	Intro::Register();
	Support::RegisterDebugRoutes();
	// Dialogs::Register();
	// History::Register();
	// Conversation::Register();
	// Eli::Register();
	// Search::Register();
	// Profile::Register();
	// Settings::Register();
}

} // namespace Core
