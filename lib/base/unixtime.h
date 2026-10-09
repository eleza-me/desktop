// Wall clock helpers. Tests may shift the clock with unixtime::setTestShiftMs.
#pragma once

#include "base/basic_types.h"

namespace base::unixtime {

// Seconds since the epoch.
[[nodiscard]] TimeId now();

// Milliseconds since the epoch.
[[nodiscard]] TimeMs nowMs();

// Adds `shift` to every reading (tests only; 0 restores the real clock).
void setTestShiftMs(TimeMs shift);

} // namespace base::unixtime
