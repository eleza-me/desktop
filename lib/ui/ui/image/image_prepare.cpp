#include "ui/image/image_prepare.h"

#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <algorithm>
#include <vector>

namespace Ui::Images {
namespace {

void BoxBlurLine(
		const uchar *source,
		uchar *target,
		int count,
		int step,
		int radius) {
	// One channel-interleaved line of `count` pixels, 4 bytes each, `step`
	// bytes between consecutive pixels.
	const auto window = radius * 2 + 1;
	int sums[4] = { 0, 0, 0, 0 };
	const auto at = [&](int index) {
		return source + std::clamp(index, 0, count - 1) * step;
	};
	for (auto i = -radius; i <= radius; ++i) {
		const auto pixel = at(i);
		for (auto c = 0; c != 4; ++c) {
			sums[c] += pixel[c];
		}
	}
	for (auto i = 0; i != count; ++i) {
		const auto to = target + i * step;
		for (auto c = 0; c != 4; ++c) {
			to[c] = uchar(sums[c] / window);
		}
		const auto remove = at(i - radius);
		const auto add = at(i + radius + 1);
		for (auto c = 0; c != 4; ++c) {
			sums[c] += int(add[c]) - int(remove[c]);
		}
	}
}

void BoxBlur(QImage &image, int radius) {
	const auto width = image.width();
	const auto height = image.height();
	if (radius <= 0 || width <= 0 || height <= 0) {
		return;
	}
	auto buffer = std::vector<uchar>(size_t(std::max(width, height)) * 4);
	const auto perLine = int(image.bytesPerLine());
	for (auto y = 0; y != height; ++y) {
		const auto line = image.scanLine(y);
		BoxBlurLine(line, buffer.data(), width, 4, radius);
		std::copy(buffer.data(), buffer.data() + width * 4, line);
	}
	auto column = std::vector<uchar>(size_t(height) * 4);
	const auto bits = image.bits();
	for (auto x = 0; x != width; ++x) {
		for (auto y = 0; y != height; ++y) {
			const auto from = bits + y * perLine + x * 4;
			std::copy(from, from + 4, column.data() + y * 4);
		}
		BoxBlurLine(column.data(), buffer.data(), height, 4, radius);
		for (auto y = 0; y != height; ++y) {
			const auto to = bits + y * perLine + x * 4;
			std::copy(buffer.data() + y * 4, buffer.data() + y * 4 + 4, to);
		}
	}
}

} // namespace

QImage PrepareCover(const QImage &image, QSize size, double ratio) {
	const auto target = QSize(
		std::max(int(size.width() * ratio), 1),
		std::max(int(size.height() * ratio), 1));
	if (image.isNull()) {
		auto result = QImage(target, QImage::Format_ARGB32_Premultiplied);
		result.fill(Qt::transparent);
		result.setDevicePixelRatio(ratio);
		return result;
	}
	const auto scaled = image.scaled(
		target,
		Qt::KeepAspectRatioByExpanding,
		Qt::SmoothTransformation);
	auto result = scaled.copy(
		(scaled.width() - target.width()) / 2,
		(scaled.height() - target.height()) / 2,
		target.width(),
		target.height()
	).convertToFormat(QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(ratio);
	return result;
}

QImage Round(QImage image, int radius) {
	return Round(std::move(image), radius, radius, radius, radius);
}

QImage Round(
		QImage image,
		int topLeft,
		int topRight,
		int bottomRight,
		int bottomLeft) {
	if (image.isNull()) {
		return image;
	}
	const auto ratio = image.devicePixelRatio();
	image = std::move(image).convertToFormat(
		QImage::Format_ARGB32_Premultiplied);
	const auto w = image.width() / ratio;
	const auto h = image.height() / ratio;
	const auto limit = std::min(w, h) / 2.;
	const auto tl = std::min(double(topLeft), limit);
	const auto tr = std::min(double(topRight), limit);
	const auto br = std::min(double(bottomRight), limit);
	const auto bl = std::min(double(bottomLeft), limit);

	auto path = QPainterPath();
	path.moveTo(tl, 0);
	path.lineTo(w - tr, 0);
	path.arcTo(QRectF(w - 2 * tr, 0, 2 * tr, 2 * tr), 90, -90);
	path.lineTo(w, h - br);
	path.arcTo(QRectF(w - 2 * br, h - 2 * br, 2 * br, 2 * br), 0, -90);
	path.lineTo(bl, h);
	path.arcTo(QRectF(0, h - 2 * bl, 2 * bl, 2 * bl), 270, -90);
	path.lineTo(0, tl);
	path.arcTo(QRectF(0, 0, 2 * tl, 2 * tl), 180, -90);
	path.closeSubpath();

	auto mask = QImage(image.size(), QImage::Format_ARGB32_Premultiplied);
	mask.setDevicePixelRatio(ratio);
	mask.fill(Qt::transparent);
	{
		auto p = QPainter(&mask);
		p.setRenderHint(QPainter::Antialiasing);
		p.fillPath(path, Qt::white);
	}
	{
		auto p = QPainter(&image);
		p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
		p.drawImage(QPointF(), mask);
	}
	return image;
}

QImage Circle(QImage image) {
	const auto ratio = image.devicePixelRatio();
	const auto side = int(std::min(image.width(), image.height()) / ratio);
	return Round(std::move(image), (side + 1) / 2);
}

QImage Blur(QImage image, int radius) {
	if (image.isNull() || radius <= 0) {
		return image;
	}
	image = std::move(image).convertToFormat(
		QImage::Format_ARGB32_Premultiplied);
	// Three box passes approximate a gaussian.
	const auto pass = std::max(radius / 2, 1);
	BoxBlur(image, pass);
	BoxBlur(image, pass);
	BoxBlur(image, std::max(radius - pass, 1));
	return image;
}

QImage BlurredPreview(const QImage &image, QSize size, double ratio) {
	if (image.isNull()) {
		return PrepareCover(image, size, ratio);
	}
	const auto small = image.scaled(
		QSize(48, 48),
		Qt::KeepAspectRatioByExpanding,
		Qt::SmoothTransformation);
	return PrepareCover(Blur(small, 6), size, ratio);
}

QImage Colorize(const QImage &mask, const QColor &color) {
	auto result = mask.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	auto p = QPainter(&result);
	p.setCompositionMode(QPainter::CompositionMode_SourceIn);
	p.fillRect(QRect(QPoint(), result.size()), color);
	p.end();
	return result;
}

} // namespace Ui::Images
