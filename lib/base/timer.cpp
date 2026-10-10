#include "base/timer.h"

namespace base {

Timer::Timer(Fn<void()> callback, QObject *parent)
: QObject(parent)
, _callback(std::move(callback)) {
	_timer.setTimerType(Qt::CoarseTimer);
	connect(&_timer, &QTimer::timeout, this, [this] {
		if (const auto callback = _callback) {
			callback();
		}
	});
}

void Timer::setCallback(Fn<void()> callback) {
	_callback = std::move(callback);
}

void Timer::callOnce(int ms) {
	_timer.setSingleShot(true);
	_timer.start(ms < 0 ? 0 : ms);
}

void Timer::callEach(int ms) {
	_timer.setSingleShot(false);
	_timer.start(ms < 1 ? 1 : ms);
}

void Timer::cancel() {
	_timer.stop();
}

bool Timer::isActive() const {
	return _timer.isActive();
}

int Timer::remainingTime() const {
	return _timer.isActive() ? _timer.remainingTime() : -1;
}

void call_delayed(int ms, QObject *guard, Fn<void()> callback) {
	if (!guard || !callback) {
		return;
	}
	QTimer::singleShot(ms < 0 ? 0 : ms, guard, [callback = std::move(callback)] {
		callback();
	});
}

} // namespace base
