#include "ui/effects/slide_animation.h"

#include "ui/style/style_core.h"
#include "styles/style_basic.h"

#include <QtGui/QPainter>

namespace Ui {

void CrossFadeAnimation::setSnapshots(QPixmap was, QPixmap now) {
	_was = std::move(was);
	_now = std::move(now);
}

void CrossFadeAnimation::start(std::function<void()> callback, int duration) {
	_animation.start(
		std::move(callback),
		0.,
		1.,
		duration ? duration : st::motionFast,
		anim::linear);
}

void CrossFadeAnimation::stop() {
	_animation.stop();
	_was = _now = QPixmap();
}

void CrossFadeAnimation::paint(QPainter &p, const QRect &rect) {
	const auto progress = _animation.value(1.);
	if (progress < 1. && !_was.isNull()) {
		p.drawPixmap(rect.topLeft(), _was);
	}
	if (!_now.isNull()) {
		p.setOpacity(progress);
		p.drawPixmap(rect.topLeft(), _now);
		p.setOpacity(1.);
	}
}

void SlideAnimation::setSnapshots(QPixmap was, QPixmap now) {
	_was = std::move(was);
	_now = std::move(now);
}

void SlideAnimation::start(
		bool back,
		std::function<void()> callback,
		int duration) {
	_back = back;
	_animation.start(
		std::move(callback),
		0.,
		1.,
		duration ? duration : st::motion,
		anim::standard);
}

void SlideAnimation::stop() {
	_animation.stop();
	_was = _now = QPixmap();
}

void SlideAnimation::paint(QPainter &p, const QRect &rect) {
	const auto progress = _animation.value(1.);
	const auto width = rect.width();
	const auto parallax = width / 4;
	// +1 when new content enters from the right on a forward transition.
	const auto direction = style::RightToLeft() ? -1 : 1;

	p.save();
	p.setClipRect(rect);
	if (_back) {
		// The previous section is revealed under the one sliding away.
		if (!_now.isNull()) {
			p.setOpacity(0.4 + 0.6 * progress);
			p.drawPixmap(
				rect.x() - int(direction * parallax * (1. - progress)),
				rect.y(),
				_now);
			p.setOpacity(1.);
		}
		if (!_was.isNull()) {
			p.drawPixmap(
				rect.x() + int(direction * width * progress),
				rect.y(),
				_was);
		}
	} else {
		if (!_was.isNull()) {
			p.setOpacity(1. - 0.6 * progress);
			p.drawPixmap(
				rect.x() - int(direction * parallax * progress),
				rect.y(),
				_was);
			p.setOpacity(1.);
		}
		if (!_now.isNull()) {
			p.drawPixmap(
				rect.x() + int(direction * width * (1. - progress)),
				rect.y(),
				_now);
		}
	}
	p.restore();
}

} // namespace Ui
