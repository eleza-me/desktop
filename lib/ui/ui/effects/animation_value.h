// Easing curves and interpolation helpers.
#pragma once

#include <QtGui/QColor>

#include <cmath>

namespace anim {

using transition = double(*)(double progress);

[[nodiscard]] inline double linear(double t) {
	return t;
}
[[nodiscard]] inline double sineInOut(double t) {
	return -0.5 * (std::cos(3.14159265358979323846 * t) - 1.);
}
[[nodiscard]] inline double easeInCubic(double t) {
	return t * t * t;
}
[[nodiscard]] inline double easeOutCubic(double t) {
	const auto inverted = 1. - t;
	return 1. - inverted * inverted * inverted;
}
[[nodiscard]] inline double easeInOutCubic(double t) {
	if (t < 0.5) {
		return 4. * t * t * t;
	}
	const auto inverted = -2. * t + 2.;
	return 1. - inverted * inverted * inverted / 2.;
}
[[nodiscard]] inline double easeOutQuint(double t) {
	const auto inverted = 1. - t;
	return 1. - inverted * inverted * inverted * inverted * inverted;
}
[[nodiscard]] inline double easeOutCirc(double t) {
	const auto shifted = t - 1.;
	return std::sqrt(1. - shifted * shifted);
}
// A little overshoot at the end (toggles, popups).
[[nodiscard]] inline double easeOutBack(double t) {
	const auto c1 = 1.70158;
	const auto c3 = c1 + 1.;
	const auto shifted = t - 1.;
	return 1. + c3 * shifted * shifted * shifted + c1 * shifted * shifted;
}
// The design's default curve, cubic-bezier(.2, .8, .2, 1): fast start, long tail.
[[nodiscard]] double standard(double t);

[[nodiscard]] inline double interpolateF(double a, double b, double ratio) {
	return a + (b - a) * ratio;
}
[[nodiscard]] inline int interpolate(int a, int b, double ratio) {
	return int(std::lround(a + (b - a) * ratio));
}
[[nodiscard]] QColor color(const QColor &a, const QColor &b, double ratio);

} // namespace anim

namespace anim {

// Whether a state change should be animated.
enum class type {
	normal,
	instant,
};

} // namespace anim
