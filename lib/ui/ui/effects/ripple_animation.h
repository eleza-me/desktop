// Material-like press ripples clipped to a mask.
//
//   // in a RippleButton this is all done for you; manual use:
//   _ripple = std::make_unique<Ui::RippleAnimation>(
//       st::defaultRipple,
//       Ui::RippleAnimation::RoundRectMask(size(), st::radiusMd),
//       [=] { update(); });
//   _ripple->add(pressPoint);     // on press
//   _ripple->lastStop();          // on release
//   _ripple->paint(p, 0, 0);      // in paintEvent
#pragma once

#include "ui/effects/animations.h"
#include "styles/style_basic.h"

#include <QtGui/QImage>

#include <deque>
#include <functional>

class QPainter;

namespace Ui {

class RippleAnimation final {
public:
	RippleAnimation(
		const style::RippleAnimation &st,
		QImage mask,
		std::function<void()> update);
	~RippleAnimation();

	// `origin` in mask coordinates.
	void add(QPoint origin, int startRadius = 0);
	// A ripple that already covers everything (keyboard activation, forced state).
	void addFading();
	// Starts fading the newest ripple out (mouse released).
	void lastStop();
	// Fades the newest ripple out right away (press cancelled).
	void lastFinish();
	void forceRepaint();

	// `x`, `y` are physical (not mirrored) coordinates of the mask.
	void paint(
		QPainter &p,
		int x,
		int y,
		const QColor *colorOverride = nullptr);

	[[nodiscard]] bool empty() const {
		return _ripples.empty();
	}

	// Masks, sizes in logical pixels.
	[[nodiscard]] static QImage MaskByDrawer(
		QSize size,
		bool filled,
		const std::function<void(QPainter &p)> &drawer);
	[[nodiscard]] static QImage RectMask(QSize size);
	[[nodiscard]] static QImage RoundRectMask(QSize size, int radius);
	[[nodiscard]] static QImage EllipseMask(QSize size);

private:
	struct Ripple {
		QPoint origin;
		int startRadius = 0;
		int maxRadius = 0;
		qint64 shownAt = 0;
		qint64 hiddenAt = 0; // 0 while pressed
		bool fading = false; // added already fully shown
	};

	void startAnimation();
	[[nodiscard]] bool callback(qint64 now);
	void clearFinished(qint64 now);

	const style::RippleAnimation &_st;
	QImage _mask;
	std::function<void()> _update;
	std::deque<Ripple> _ripples;
	Animations::Basic _animation;

};

} // namespace Ui
