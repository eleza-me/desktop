// Core::SystemReducesMotion() on Windows (core_settings.h). Compiled on
// Windows only.
#include "core/core_settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // !WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif // !NOMINMAX
#include <windows.h>

namespace Core {

bool SystemReducesMotion() {
	// Settings > Accessibility > Visual effects > «Animation effects» (the
	// older «Show animations in Windows»); what browsers read for
	// prefers-reduced-motion.
	auto enabled = BOOL(TRUE);
	if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) {
		return false;
	}
	return !enabled;
}

} // namespace Core
