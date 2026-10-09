// Loading indicators: an infinite spinner and a progress ring.
//
//   Ui::InfiniteRadialAnimation _loading([=] { update(); });
//   _loading.start();  ...  _loading.draw(p, rect, 2, st::brandPrimary->c);
//
// Or the ready widget: new Ui::LoadingSpinner(parent, 24, st::brandPrimary).
#pragma once

#include "ui/effects/animations.h"
#include "ui/rp_widget.h"

#include <functional>

class QPainter;

namespace Ui {

class InfiniteRadialAnimation final {
public:
	explicit InfiniteRadialAnimation(std::function<void()> update);

	void start();
	void stop();
	[[nodiscard]] bool animating() const {
		return _animation.animating();
	}

	// Draws an arc inscribed into `rect` (square) with the given line thickness.
	void draw(
		QPainter &p,
		const QRect &rect,
		double thickness,
		const QColor &color) const;

	// Draws a static frame for a given time (used when animations are disabled).
	static void DrawFrame(
		QPainter &p,
		const QRect &rect,
		double thickness,
		const QColor &color,
		qint64 elapsed);

private:
	std::function<void()> _update;
	Animations::Basic _animation;
	qint64 _started = 0;

};

// A ring showing 0..1 progress, with a slow rotation while it is not complete.
class RadialProgressAnimation final {
public:
	explicit RadialProgressAnimation(std::function<void()> update);

	void setProgress(double progress, bool animated = true);
	[[nodiscard]] double progress() const;
	void start();
	void stop();

	void draw(
		QPainter &p,
		const QRect &rect,
		double thickness,
		const QColor &color) const;

private:
	std::function<void()> _update;
	Animations::Basic _rotation;
	Animations::Simple _value;
	double _progress = 0.;
	qint64 _started = 0;

};

class LoadingSpinner final : public RpWidget {
public:
	LoadingSpinner(QWidget *parent, int size, style::color color);

	void setThickness(double thickness);

protected:
	void paintEvent(QPaintEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;

private:
	style::color _color;
	double _thickness = 2.5;
	InfiniteRadialAnimation _animation;

};

} // namespace Ui
