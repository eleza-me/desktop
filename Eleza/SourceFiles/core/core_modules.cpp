#include "core/core_modules.h"

#include "dialogs/dialogs_module.h"
#include "history/history_module.h"
#include "conversation/conversation_module.h"
#include "eli/eli_module.h"
#include "intro/intro_module.h"
#include "profile/profile_module.h"
#include "search/search_module.h"
#include "settings/settings_module.h"
#include "support/session_services.h"

// Feature modules: include the header that declares `Register()` and call it below.
// (Until a tab has a module it shows a placeholder, and so does a route nobody
// registered a handler for: see window/window_registry.h.)

namespace Core {

void RegisterModules() {
	Intro::Register();
	Support::RegisterDebugRoutes();
	Dialogs::Register();
	History::Register();
	Conversation::Register();
	Eli::Register();
	Search::Register();
	Profile::Register();
	Settings::Register();
}

} // namespace Core
