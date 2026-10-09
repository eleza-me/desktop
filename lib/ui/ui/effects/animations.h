// Value animations driven by one shared frame timer.
//
//   Ui::Animations::Simple _a;
//   _a.start([=] { update(); }, 0., 1., st::motionFast, anim::easeOutCubic);
//   ... in paintEvent: const auto progress = _a.value(_shown ? 1. : 0.);
//
// `value(final)` returns the current value while animating and `final` otherwise, so a
// widget only stores its logical state. The callback is invoked on every frame and once
// more after the animation finished. Destroying the object stops it.
#pragma once

#include "ui/effects/animation_value.h"

#include <QtCore/QObject>

#include <functional>
#include <memory>

namespace Ui::Animations {

class Manager;

// Frame callback animation: `callback(now)` returns false to stop.
class Basic final {
public:
	Basic() = default;
	explicit Basic(std::function<bool(qint64 now)> callback);
	Basic(const Basic &) = delete;
	Basic &operator=(const Basic &) = delete;
	~Basic();

	void init(std::function<bool(qint64 now)> callback);
	void start();
	void stop();
	[[nodiscard]] bool animating() const {
		return _animating;
	}
	[[nodiscard]] qint64 started() const {
		return _started;
	}

private:
	friend class Manager;

	[[nodiscard]] bool call(qint64 now);

	std::function<bool(qint64 now)> _callback;
	qint64 _started = 0;
	bool _animating = false;

};

class Simple final {
public:
	Simple() = default;
	Simple(const Simple &) = delete;
	Simple &operator=(const Simple &) = delete;
	~Simple() = default;

	void start(
		std::function<void()> callback,
		double from,
		double to,
		int duration,
		anim::transition transition = anim::linear);
	// Retargets a running animation keeping the current value as the start.
	void change(
		double to,
		int duration,
		anim::transition transition = anim::linear);
	void stop();
	[[nodiscard]] bool animating() const;
	[[nodiscard]] double value(double final) const;

private:
	struct Data;

	std::unique_ptr<Data, void(*)(Data*)> _data = { nullptr, nullptr };

};

// The shared timer. Created on first use on the GUI thread.
class Manager final : public QObject {
public:
	[[nodiscard]] static Manager &Instance();

	void start(Basic *animation);
	void stop(Basic *animation);

protected:
	void timerEvent(QTimerEvent *e) override;

private:
	Manager();

	void update();

	std::vector<Basic*> _active;
	std::vector<Basic*> _starting;
	int _timerId = 0;
	bool _updating = false;
	bool _removedWhileUpdating = false;

};

} // namespace Ui::Animations
