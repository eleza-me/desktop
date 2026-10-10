// Image helpers: rounding, blurring, scaling for display.
#pragma once

#include <QtGui/QImage>
#include <QtGui/QPixmap>

namespace Ui::Images {

// Scales and center-crops `image` to cover `size` logical pixels at `ratio`.
[[nodiscard]] QImage PrepareCover(
	const QImage &image,
	QSize size,
	double ratio);

// Clips the image to a rounded rectangle (radius in logical pixels of the image).
[[nodiscard]] QImage Round(QImage image, int radius);
// Different radii per corner: top-left, top-right, bottom-right, bottom-left.
[[nodiscard]] QImage Round(
	QImage image,
	int topLeft,
	int topRight,
	int bottomRight,
	int bottomLeft);
[[nodiscard]] QImage Circle(QImage image);

// Fast gaussian-like blur (three box passes). `radius` in device pixels.
[[nodiscard]] QImage Blur(QImage image, int radius);

// A heavily blurred small copy, for spoilers and media placeholders.
[[nodiscard]] QImage BlurredPreview(const QImage &image, QSize size, double ratio);

// Multiplies every pixel by `color` (keeps the alpha shape): for masks.
[[nodiscard]] QImage Colorize(const QImage &mask, const QColor &color);

} // namespace Ui::Images
