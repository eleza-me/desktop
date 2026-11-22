#include "base/unixtime.h"

#include <QtCore/QDateTime>

#include <atomic>

namespace base::unixtime {
namespace {

std::atomic<TimeMs> TestShift = 0;
std::atomic<TimeMs> ServerOffset = 0;

// An answer slower than this says little about where in it the server read
// its clock.
constexpr auto kMaxUsefulRttMs = TimeMs(30'000);

} // namespace

TimeId now() {
	return nowMs() / 1000;
}

TimeMs nowMs() {
	return QDateTime::currentMSecsSinceEpoch() + TestShift.load();
}

void setTestShiftMs(TimeMs shift) {
	TestShift = shift;
}

TimeId serverNow() {
	return serverNowMs() / 1000;
}

TimeMs serverNowMs() {
	return nowMs() + ServerOffset.load();
}

TimeMs serverOffsetMs() {
	return ServerOffset.load();
}

void setServerOffsetMs(TimeMs offset) {
	ServerOffset = offset;
}

std::optional<TimeMs> OffsetFromHello(
		TimeMs serverTimeMs,
		TimeMs receivedAtMs,
		TimeMs rttMs) {
	if (serverTimeMs <= 0 || rttMs < 0 || rttMs > kMaxUsefulRttMs) {
		return std::nullopt;
	}
	return serverTimeMs + rttMs / 2 - receivedAtMs;
}

} // namespace base::unixtime
