// A callback timer on the Qt event loop of the thread that owns it.
//
//   base::Timer _timer{ [=] { check(); } };
//   _timer.callOnce(500);     // single shot (restarts when already active)
//   _timer.callEach(5000);    // repeating
//   _timer.cancel();
//
// base::call_delayed(ms, guard, fn) runs fn once unless `guard` died meanwhile.
#pragma once

#include "base/basic_types.h"

#include <QtCore/QObject>
#include <QtCore/QTimer>

namespace base {

class Timer final : public QObject {
public:
	explicit Timer(Fn<void()> callback = nullptr, QObject *parent = nullptr);

	void setCallback(Fn<void()> callback);

	void callOnce(int ms);
	void callEach(int ms);
	void cancel();

	[[nodiscard]] bool isActive() const;
	// Milliseconds until the next shot, or -1 when inactive.
	[[nodiscard]] int remainingTime() const;

private:
	QTimer _timer;
	Fn<void()> _callback;

};

void call_delayed(int ms, QObject *guard, Fn<void()> callback);

} // namespace base
