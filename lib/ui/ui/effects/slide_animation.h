// Transitions between two snapshots of a section: cross-fade and slide.
//
//   Ui::SlideAnimation _slide;
//   _slide.setSnapshots(Ui::GrabWidget(old), Ui::GrabWidget(now));
//   _slide.start(back, [=] { update(); if (!_slide.animating()) showChildren(); });
//   ... in paintEvent: if (_slide.animating()) { _slide.paint(p, rect()); return; }
#pragma once

#include "ui/effects/animations.h"

#include <QtGui/QPixmap>

#include <functional>

class QPainter;

namespace Ui {

// The new content fades in over the old one.
class CrossFadeAnimation final {
public:
	void setSnapshots(QPixmap was, QPixmap now);
	void start(std::function<void()> callback, int duration = 0);
	void stop();
	[[nodiscard]] bool animating() const {
		return _animation.animating();
	}
	void paint(QPainter &p, const QRect &rect);

private:
	QPixmap _was;
	QPixmap _now;
	Animations::Simple _animation;

};

// The new section slides in from the trailing side (forward) or the old one slides
// out to it (back), like pushing / popping a navigation stack. RTL-aware.
class SlideAnimation final {
public:
	void setSnapshots(QPixmap was, QPixmap now);
	void start(bool back, std::function<void()> callback, int duration = 0);
	void stop();
	[[nodiscard]] bool animating() const {
		return _animation.animating();
	}
	void paint(QPainter &p, const QRect &rect);

private:
	QPixmap _was;
	QPixmap _now;
	bool _back = false;
	Animations::Simple _animation;

};

} // namespace Ui
