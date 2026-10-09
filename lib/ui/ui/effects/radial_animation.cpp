#include "ui/effects/radial_animation.h"

#include "ui/ui_utility.h"
#include "ui/painter.h"

#include <cmath>

namespace Ui {
namespace {

constexpr auto kFullCircle = 360 * 16;
constexpr auto kRotationPeriod = 1100; // ms per turn
constexpr auto kBreathPeriod = 1500; // ms per grow / shrink cycle

void DrawArc(
		QPainter &p,
		const QRect &rect,
		double thickness,
		const QColor &color,
		int from,
		int length) {
	auto hq = PainterHighQualityEnabler(p);
	auto pen = QPen(color);
	pen.setWidthF(thickness);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	const auto half = thickness / 2.;
	p.drawArc(
		QRectF(rect).marginsRemoved(QMarginsF(half, half, half, half)),
		from,
		length);
}

} // namespace

InfiniteRadialAnimation::InfiniteRadialAnimation(std::function<void()> update)
: _update(std::move(update))
, _animation([this](qint64) {
	if (_update) {
		_update();
	}
	return true;
}) {
}

void InfiniteRadialAnimation::start() {
	if (!_animation.animating()) {
		_started = Ui::Now();
		if (!Ui::AnimationsDisabled()) {
			_animation.start();
		}
	}
}

void InfiniteRadialAnimation::stop() {
	_animation.stop();
}

void InfiniteRadialAnimation::DrawFrame(
		QPainter &p,
		const QRect &rect,
		double thickness,
		const QColor &color,
		qint64 elapsed) {
	const auto turn = double(elapsed % kRotationPeriod) / kRotationPeriod;
	const auto breath = double(elapsed % kBreathPeriod) / kBreathPeriod;
	// The arc length breathes between 60 and 270 degrees.
	const auto wave = 0.5 - 0.5 * std::cos(breath * 2. * 3.14159265358979);
	const auto length = int((60. + 210. * wave) * 16.);
	const auto from = int(kFullCircle * (0.25 - turn)) - int(wave * 90. * 16.);
	DrawArc(p, rect, thickness, color, from, -length);
}

void InfiniteRadialAnimation::draw(
		QPainter &p,
		const QRect &rect,
		double thickness,
		const QColor &color) const {
	const auto elapsed = Ui::AnimationsDisabled()
		? qint64(kBreathPeriod / 3)
		: (Ui::Now() - _started);
	DrawFrame(p, rect, thickness, color, elapsed);
}

RadialProgressAnimation::RadialProgressAnimation(std::function<void()> update)
: _update(std::move(update))
, _rotation([this](qint64) {
	if (_update) {
		_update();
	}
	return true;
}) {
}

void RadialProgressAnimation::setProgress(double progress, bool animated) {
	progress = std::clamp(progress, 0., 1.);
	if (_progress == progress) {
		return;
	}
	const auto from = this->progress();
	_progress = progress;
	if (animated) {
		_value.start(_update, from, progress, 200, anim::easeOutCubic);
	} else {
		_value.stop();
		if (_update) {
			_update();
		}
	}
}

double RadialProgressAnimation::progress() const {
	return _value.value(_progress);
}

void RadialProgressAnimation::start() {
	if (!_rotation.animating()) {
		_started = Ui::Now();
		if (!Ui::AnimationsDisabled()) {
			_rotation.start();
		}
	}
}

void RadialProgressAnimation::stop() {
	_rotation.stop();
}

void RadialProgressAnimation::draw(
		QPainter &p,
		const QRect &rect,
		double thickness,
		const QColor &color) const {
	const auto elapsed = _rotation.animating() ? (Ui::Now() - _started) : 0;
	const auto turn = double(elapsed % (kRotationPeriod * 2))
		/ (kRotationPeriod * 2);
	const auto shown = std::max(progress(), 0.04);
	DrawArc(
		p,
		rect,
		thickness,
		color,
		int(kFullCircle * (0.25 - turn)),
		-int(kFullCircle * shown));
}

LoadingSpinner::LoadingSpinner(QWidget *parent, int size, style::color color)
: RpWidget(parent)
, _color(color)
, _thickness(std::max(2., size / 10.))
, _animation([this] { update(); }) {
	resize(size, size);
	setAttribute(Qt::WA_TransparentForMouseEvents);
}

void LoadingSpinner::setThickness(double thickness) {
	_thickness = thickness;
	update();
}

void LoadingSpinner::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = QPainter(this);
	_animation.draw(p, rect(), _thickness, _color->c);
}

void LoadingSpinner::showEvent(QShowEvent *e) {
	_animation.start();
	RpWidget::showEvent(e);
}

void LoadingSpinner::hideEvent(QHideEvent *e) {
	_animation.stop();
	RpWidget::hideEvent(e);
}

} // namespace Ui
