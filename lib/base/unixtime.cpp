#include "base/unixtime.h"

#include <QtCore/QDateTime>

#include <atomic>

namespace base::unixtime {
namespace {

std::atomic<TimeMs> TestShift = 0;

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

} // namespace base::unixtime
