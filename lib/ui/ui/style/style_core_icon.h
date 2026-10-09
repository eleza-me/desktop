// style::icon — a tinted vector icon from :/icons/<name>.svg, crisp at any pixel ratio.
//
// Declared in .style files:
//   chatSendIcon: icon {{ "send", brandPrimary }};          // 24px
//   menuIcon: icon {{ "more", textSecondary, 20px }};       // custom size
//   backIcon: icon {{ "back:mirror-ltr", textPrimary }};    // drawn for RTL, mirrored in LTR
//   nextIcon: icon {{ "chevron-right:mirror-rtl", ... }};   // drawn for LTR, mirrored in RTL
//
// Painting:
//   st::chatSendIcon.paint(p, x, y, width());               // x from the leading side (RTL-aware)
//   st::chatSendIcon.paintInCenter(p, rect());
//   st::menuIcon.paint(p, x, y, width(), st::statusError->c); // color override
#pragma once

#include "ui/style/style_core_color.h"

#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtCore/QSize>
#include <QtGui/QPixmap>

class QPainter;

namespace style {
namespace internal {

class IconData;

void StartIcons();

} // namespace internal

class icon {
public:
	icon() = default;
	icon(const QString &name, color tint, int size);

	[[nodiscard]] bool empty() const {
		return _data == nullptr;
	}
	[[nodiscard]] int width() const;
	[[nodiscard]] int height() const;
	[[nodiscard]] QSize size() const;

	// `x` is counted from the left in LTR and from the right in RTL (inside `outerw`).
	void paint(QPainter &p, int x, int y, int outerw) const;
	void paint(QPainter &p, const QPoint &pos, int outerw) const;
	void paint(QPainter &p, int x, int y, int outerw, const QColor &colorOverride) const;
	void paintInCenter(QPainter &p, const QRect &outer) const;
	void paintInCenter(QPainter &p, const QRect &outer, const QColor &colorOverride) const;

	// A rasterised copy (device pixel ratio set on the pixmap).
	[[nodiscard]] QPixmap pixmap(double devicePixelRatio) const;
	[[nodiscard]] QPixmap pixmap(double devicePixelRatio, const QColor &colorOverride) const;

private:
	const internal::IconData *_data = nullptr;

};

// Rasterises :/icons/<name>.svg with a tint, without declaring it in a .style file.
[[nodiscard]] QPixmap IconPixmap(
	const QString &name,
	const QColor &tint,
	int size,
	double devicePixelRatio);

} // namespace style
