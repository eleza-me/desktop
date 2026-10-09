#include "support/brand_mark.h"

#include "ui/painter.h"
#include "styles/palette.h"

namespace Support {

void PaintBrandMark(QPainter &p, const QRect &rect) {
	PaintBrandMark(p, rect, st::brandMint->c, st::bgNight->c);
}

void PaintBrandMark(
		QPainter &p,
		const QRect &rect,
		const QColor &almond,
		const QColor &pupil,
		const QColor &glint) {
	// Designed on a 140 x 112 grid.
	const auto scale = std::min(rect.width() / 140., rect.height() / 112.);
	const auto width = 140. * scale;
	const auto height = 112. * scale;
	const auto left = rect.x() + (rect.width() - width) / 2.;
	const auto top = rect.y() + (rect.height() - height) / 2.;
	const auto big = 40. * scale;
	const auto small = 10. * scale;

	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);

	// Three round corners and a sharp bottom left one.
	auto path = QPainterPath();
	path.moveTo(left + big, top);
	path.lineTo(left + width - big, top);
	path.arcTo(left + width - 2 * big, top, 2 * big, 2 * big, 90, -90);
	path.lineTo(left + width, top + height - big);
	path.arcTo(
		left + width - 2 * big,
		top + height - 2 * big,
		2 * big,
		2 * big,
		0,
		-90);
	path.lineTo(left + small, top + height);
	path.arcTo(left, top + height - 2 * small, 2 * small, 2 * small, 270, -90);
	path.lineTo(left, top + big);
	path.arcTo(left, top, 2 * big, 2 * big, 180, -90);
	path.closeSubpath();
	p.fillPath(path, almond);

	p.setBrush(pupil);
	p.drawEllipse(QRectF(
		left + 42. * scale,
		top + 28. * scale,
		56. * scale,
		56. * scale));
	p.setBrush(glint);
	p.drawEllipse(QRectF(
		left + 54. * scale,
		top + 36. * scale,
		16. * scale,
		16. * scale));
}

} // namespace Support
