// Settings → Privacy → «استوری»: who may reply and react (everyone /
// people I follow / off), the close-friends list and the hide-from list
// (add through a people picker, remove from the row).
#pragma once

#include "window/window_registry.h"

namespace Stories {

[[nodiscard]] Window::SectionFactory SettingsFactory();

} // namespace Stories
