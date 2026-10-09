// The one place where feature modules plug into the app.
//
// A module (intro, dialogs, eli, search, profile, settings…) exposes a single function,
// e.g. `void Dialogs::Register();`, which calls Window::RegisterTab / RegisterIntro /
// RegisterRouteHandler (window/window_registry.h). Add the call to RegisterModules()
// in core_modules.cpp — nothing else in core/ or window/ needs to change.
#pragma once

namespace Core {

void RegisterModules();

} // namespace Core
