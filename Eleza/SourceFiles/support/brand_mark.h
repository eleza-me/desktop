// The Eleza mark: an almond-shaped eye (a speech bubble with a pupil).
//
//   Support::PaintBrandMark(p, rect);                       // mint on night colors
//   Support::PaintBrandMark(p, rect, st::brandPrimary->c, st::bgCanvas->c);
//
// The mark keeps its 5:4 proportion and is centered inside `rect`. It is not
// mirrored in RTL (a logo has one shape).
#pragma once

#include <QtCore/QRect>
#include <QtGui/QColor>

class QPainter;

namespace Support {

void PaintBrandMark(QPainter &p, const QRect &rect);
void PaintBrandMark(
	QPainter &p,
	const QRect &rect,
	const QColor &almond,
	const QColor &pupil,
	const QColor &glint = QColor(255, 255, 255));

} // namespace Support
