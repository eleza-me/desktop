#include "ui/effects/ripple_animation.h"

#include "ui/ui_utility.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>

#include <algorithm>
#include <cmath>

namespace Ui {
namespace {

[[nodiscard]] double MaskRatio() {
	return qGuiApp ? qGuiApp->devicePixelRatio() : 1.;
}

} // namespace

RippleAnimation::RippleAnimation(
	const style::RippleAnimation &st,
	QImage mask,
	std::function<void()> update)
: _st(st)
, _mask(std::move(mask))
, _update(std::move(update))
, _animation([this](qint64 now) { return callback(now); }) {
}

RippleAnimation::~RippleAnimation() = default;

void RippleAnimation::add(QPoint origin, int startRadius) {
	lastStop();
	const auto ratio = _mask.devicePixelRatio();
	const auto w = int(_mask.width() / ratio);
	const auto h = int(_mask.height() / ratio);
	const auto dx = std::max(origin.x(), w - origin.x());
	const auto dy = std::max(origin.y(), h - origin.y());
	auto ripple = Ripple();
	ripple.origin = origin;
	ripple.startRadius = startRadius;
	ripple.maxRadius = int(std::ceil(std::sqrt(double(dx * dx + dy * dy))));
	ripple.shownAt = Ui::Now();
	_ripples.push_back(ripple);
	startAnimation();
}

void RippleAnimation::addFading() {
	lastStop();
	const auto ratio = _mask.devicePixelRatio();
	auto ripple = Ripple();
	ripple.origin = QPoint(
		int(_mask.width() / ratio / 2),
		int(_mask.height() / ratio / 2));
	ripple.maxRadius = int(_mask.width() / ratio + _mask.height() / ratio);
	ripple.startRadius = ripple.maxRadius;
	ripple.shownAt = Ui::Now() - _st.showDuration;
	ripple.fading = true;
	_ripples.push_back(ripple);
	startAnimation();
}

void RippleAnimation::lastStop() {
	if (!_ripples.empty() && !_ripples.back().hiddenAt) {
		_ripples.back().hiddenAt = Ui::Now();
		startAnimation();
	}
}

void RippleAnimation::lastFinish() {
	if (!_ripples.empty()) {
		auto &last = _ripples.back();
		const auto now = Ui::Now();
		if (!last.hiddenAt) {
			last.hiddenAt = now;
		}
		last.shownAt = std::min(last.shownAt, now - _st.showDuration);
		startAnimation();
	}
}

void RippleAnimation::forceRepaint() {
	if (_update) {
		_update();
	}
}

void RippleAnimation::startAnimation() {
	if (Ui::AnimationsDisabled()) {
		clearFinished(Ui::Now() + _st.showDuration + _st.hideDuration);
		if (_update) {
			_update();
		}
		return;
	}
	if (!_animation.animating()) {
		_animation.start();
	}
	if (_update) {
		_update();
	}
}

void RippleAnimation::clearFinished(qint64 now) {
	while (!_ripples.empty()) {
		const auto &first = _ripples.front();
		if (first.hiddenAt && now >= first.hiddenAt + _st.hideDuration) {
			_ripples.pop_front();
		} else {
			break;
		}
	}
}

bool RippleAnimation::callback(qint64 now) {
	clearFinished(now);
	// Keep running while something still grows or fades.
	auto active = false;
	for (const auto &ripple : _ripples) {
		if (ripple.hiddenAt || now < ripple.shownAt + _st.showDuration) {
			active = true;
			break;
		}
	}
	const auto update = _update;
	if (update) {
		update();
	}
	return active;
}

void RippleAnimation::paint(
		QPainter &p,
		int x,
		int y,
		const QColor *colorOverride) {
	if (_ripples.empty() || _mask.isNull()) {
		return;
	}
	const auto now = Ui::Now();
	clearFinished(now);
	if (_ripples.empty()) {
		return;
	}
	const auto ratio = _mask.devicePixelRatio();
	const auto &color = colorOverride ? *colorOverride : _st.color->c;

	auto frame = QImage(_mask.size(), QImage::Format_ARGB32_Premultiplied);
	frame.setDevicePixelRatio(ratio);
	frame.fill(Qt::transparent);
	{
		auto q = QPainter(&frame);
		q.setRenderHint(QPainter::Antialiasing);
		q.setPen(Qt::NoPen);
		for (const auto &ripple : _ripples) {
			const auto shown = _st.showDuration
				? std::clamp(
					double(now - ripple.shownAt) / _st.showDuration,
					0.,
					1.)
				: 1.;
			const auto hidden = (ripple.hiddenAt && _st.hideDuration)
				? std::clamp(
					double(now - ripple.hiddenAt) / _st.hideDuration,
					0.,
					1.)
				: (ripple.hiddenAt ? 1. : 0.);
			const auto radius = anim::interpolateF(
				ripple.startRadius,
				ripple.maxRadius,
				anim::easeOutCubic(shown));
			q.setOpacity(1. - hidden);
			q.setBrush(color);
			q.drawEllipse(QPointF(ripple.origin), radius, radius);
		}
		q.setOpacity(1.);
		q.setCompositionMode(QPainter::CompositionMode_DestinationIn);
		q.drawImage(QPoint(), _mask);
	}
	p.drawImage(QPoint(x, y), frame);
}

QImage RippleAnimation::MaskByDrawer(
		QSize size,
		bool filled,
		const std::function<void(QPainter &p)> &drawer) {
	const auto ratio = MaskRatio();
	auto result = QImage(
		QSize(
			std::max(int(std::ceil(size.width() * ratio)), 1),
			std::max(int(std::ceil(size.height() * ratio)), 1)),
		QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(ratio);
	result.fill(filled ? QColor(255, 255, 255) : QColor(0, 0, 0, 0));
	if (drawer) {
		auto p = QPainter(&result);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 255, 255));
		drawer(p);
	}
	return result;
}

QImage RippleAnimation::RectMask(QSize size) {
	return MaskByDrawer(size, true, nullptr);
}

QImage RippleAnimation::RoundRectMask(QSize size, int radius) {
	return MaskByDrawer(size, false, [&](QPainter &p) {
		p.drawRoundedRect(
			QRect(QPoint(), size),
			std::min(radius, size.height() / 2),
			std::min(radius, size.height() / 2));
	});
}

QImage RippleAnimation::EllipseMask(QSize size) {
	return MaskByDrawer(size, false, [&](QPainter &p) {
		p.drawEllipse(QRect(QPoint(), size));
	});
}

} // namespace Ui
