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

// The server's clock: the system clock corrected by the offset the last
// realtime `hello` measured. Every deadline the server sets (expiry, edit /
// delete windows, mutes, countdowns) is compared with this, so a system
// clock that is off (a stale time zone fixed by hand) does not move them.
[[nodiscard]] TimeId serverNow();
[[nodiscard]] TimeMs serverNowMs();

// server time - system time, in ms (0 until the first `hello`).
[[nodiscard]] TimeMs serverOffsetMs();
void setServerOffsetMs(TimeMs offset);

// The offset a `hello` carrying `serverTimeMs` gives when it was received at
// `receivedAtMs`, `rttMs` after the request it answers was sent (the
// server's reading is taken about half way). Empty for a missing reading.
[[nodiscard]] std::optional<TimeMs> OffsetFromHello(
	TimeMs serverTimeMs,
	TimeMs receivedAtMs,
	TimeMs rttMs);

} // namespace base::unixtime
