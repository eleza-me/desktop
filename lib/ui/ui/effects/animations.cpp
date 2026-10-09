#include "ui/effects/animations.h"

#include "ui/ui_utility.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QTimerEvent>

#include <algorithm>

namespace anim {
namespace {

// Solves cubic-bezier(x1, y1, x2, y2) for y at the given x.
[[nodiscard]] double CubicBezier(
		double x1,
		double y1,
		double x2,
		double y2,
		double x) {
	const auto cx = 3. * x1;
	const auto bx = 3. * (x2 - x1) - cx;
	const auto ax = 1. - cx - bx;
	const auto cy = 3. * y1;
	const auto by = 3. * (y2 - y1) - cy;
	const auto ay = 1. - cy - by;
	auto t = x;
	for (auto i = 0; i != 8; ++i) {
		const auto current = ((ax * t + bx) * t + cx) * t - x;
		const auto derivative = (3. * ax * t + 2. * bx) * t + cx;
		if (std::abs(current) < 1e-6 || std::abs(derivative) < 1e-6) {
			break;
		}
		t -= current / derivative;
	}
	t = std::clamp(t, 0., 1.);
	return ((ay * t + by) * t + cy) * t;
}

} // namespace

double standard(double t) {
	return CubicBezier(0.2, 0.8, 0.2, 1., t);
}

QColor color(const QColor &a, const QColor &b, double ratio) {
	const auto r = std::clamp(ratio, 0., 1.);
	return QColor(
		interpolate(a.red(), b.red(), r),
		interpolate(a.green(), b.green(), r),
		interpolate(a.blue(), b.blue(), r),
		interpolate(a.alpha(), b.alpha(), r));
}

} // namespace anim

namespace Ui::Animations {
namespace {

constexpr auto kFrameInterval = 16;

} // namespace

struct Simple::Data {
	Basic animation;
	std::function<void()> callback;
	double from = 0.;
	double to = 0.;
	double value = 0.;
	int duration = 0;
	anim::transition transition = anim::linear;
	// Set while the callback runs, so a callback may destroy / restart us.
	bool *destroyed = nullptr;
	bool restarted = false;
};

Basic::Basic(std::function<bool(qint64 now)> callback)
: _callback(std::move(callback)) {
}

Basic::~Basic() {
	stop();
}

void Basic::init(std::function<bool(qint64 now)> callback) {
	_callback = std::move(callback);
}

void Basic::start() {
	_started = Ui::Now();
	if (!_animating) {
		_animating = true;
		Manager::Instance().start(this);
	}
}

void Basic::stop() {
	if (_animating) {
		_animating = false;
		Manager::Instance().stop(this);
	}
}

bool Basic::call(qint64 now) {
	return _callback && _callback(now);
}

void Simple::start(
		std::function<void()> callback,
		double from,
		double to,
		int duration,
		anim::transition transition) {
	if (!_data) {
		_data = std::unique_ptr<Data, void(*)(Data*)>(
			new Data(),
			[](Data *data) {
				if (data->destroyed) {
					*data->destroyed = true;
				}
				delete data;
			});
		const auto raw = _data.get();
		raw->animation.init([raw](qint64 now) {
			const auto passed = now - raw->animation.started();
			const auto finished = (passed >= raw->duration);
			const auto progress = finished
				? 1.
				: (double(passed) / raw->duration);
			raw->value = finished
				? raw->to
				: anim::interpolateF(
					raw->from,
					raw->to,
					raw->transition(progress));
			if (finished) {
				raw->animation.stop();
			}
			auto destroyed = false;
			const auto was = raw->destroyed;
			raw->destroyed = &destroyed;
			raw->restarted = false;
			if (const auto copy = raw->callback) {
				copy();
			}
			if (destroyed) {
				if (was) {
					*was = true;
				}
				return false;
			}
			raw->destroyed = was;
			return !finished || raw->restarted;
		});
	}
	_data->callback = std::move(callback);
	_data->from = from;
	_data->to = to;
	_data->value = from;
	_data->duration = duration;
	_data->transition = transition;
	_data->restarted = true;
	if (duration <= 0 || Ui::AnimationsDisabled()) {
		_data->value = to;
		_data->animation.stop();
		_data->restarted = false;
		// Report the final state asynchronously-free: the caller is in the
		// middle of changing its state and repaints itself anyway.
		return;
	}
	_data->animation.start();
}

void Simple::change(double to, int duration, anim::transition transition) {
	if (!_data || !_data->animation.animating()) {
		return;
	}
	_data->from = _data->value;
	_data->to = to;
	_data->duration = duration;
	_data->transition = transition;
	_data->restarted = true;
	_data->animation.start();
}

void Simple::stop() {
	if (_data) {
		_data->animation.stop();
		_data->restarted = false;
	}
}

bool Simple::animating() const {
	return _data && _data->animation.animating();
}

double Simple::value(double final) const {
	return animating() ? _data->value : final;
}

Manager &Manager::Instance() {
	static const auto result = new Manager();
	return *result;
}

Manager::Manager() : QObject(QCoreApplication::instance()) {
}

void Manager::start(Basic *animation) {
	if (_updating) {
		_starting.push_back(animation);
	} else {
		_active.push_back(animation);
	}
	if (!_timerId) {
		_timerId = startTimer(kFrameInterval, Qt::PreciseTimer);
	}
}

void Manager::stop(Basic *animation) {
	const auto remove = [&](std::vector<Basic*> &list) {
		const auto i = std::find(list.begin(), list.end(), animation);
		if (i == list.end()) {
			return false;
		} else if (_updating && &list == &_active) {
			*i = nullptr;
			_removedWhileUpdating = true;
		} else {
			list.erase(i);
		}
		return true;
	};
	if (!remove(_active)) {
		remove(_starting);
	}
}

void Manager::timerEvent(QTimerEvent *e) {
	if (e->timerId() == _timerId) {
		update();
	}
}

void Manager::update() {
	if (_updating) {
		return;
	}
	const auto now = Ui::Now();
	_updating = true;
	// Callbacks may start / stop / destroy animations, iterate by index.
	for (auto i = size_t(0); i < _active.size(); ++i) {
		const auto animation = _active[i];
		if (!animation) {
			continue;
		}
		const auto keep = animation->call(now);
		if (!keep && i < _active.size() && _active[i] == animation) {
			animation->_animating = false;
			_active[i] = nullptr;
			_removedWhileUpdating = true;
		}
	}
	_updating = false;
	if (_removedWhileUpdating) {
		_removedWhileUpdating = false;
		_active.erase(
			std::remove(_active.begin(), _active.end(), nullptr),
			_active.end());
	}
	if (!_starting.empty()) {
		_active.insert(_active.end(), _starting.begin(), _starting.end());
		_starting.clear();
	}
	if (_active.empty() && _timerId) {
		killTimer(_timerId);
		_timerId = 0;
	}
}

} // namespace Ui::Animations
