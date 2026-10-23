#include "core/core_modules.h"

#ifdef ELEZA_ENABLE_CALLS
#include "calls/calls_module.h"
#endif // ELEZA_ENABLE_CALLS
#include "dialogs/dialogs_module.h"
#include "history/history_module.h"
#include "conversation/conversation_module.h"
#include "eli/eli_module.h"
#include "groups/groups_module.h"
#include "intro/intro_module.h"
#include "media/media_module.h"
#include "profile/profile_module.h"
#include "search/search_module.h"
#include "settings/settings_module.h"
#include "stories/stories_module.h"
#include "support/session_services.h"

// Feature modules: include the header that declares `Register()` and call it below.
// (Until a tab has a module it shows a placeholder, and so does a route nobody
// registered a handler for: see window/window_registry.h.)

namespace Core {

void RegisterModules() {
	Intro::Register();
#ifdef ELEZA_DEBUG_TOOLS
	Support::RegisterDebugRoutes();
#endif // ELEZA_DEBUG_TOOLS
	Dialogs::Register();
	// Before the chat screen: it takes chat/<id>?show=media.
	Media::Register();
	History::Register();
	Conversation::Register();
	Groups::Register();
	Eli::Register();
	Search::Register();
	Profile::Register();
	Settings::Register();
	Stories::Register();
#ifdef ELEZA_ENABLE_CALLS
	// Voice calls: absent in a build with -DELEZA_ENABLE_CALLS=OFF (nothing
	// then registers the hooks of window/window_calls.h: no call buttons).
	Calls::Register();
#endif // ELEZA_ENABLE_CALLS
}

} // namespace Core
