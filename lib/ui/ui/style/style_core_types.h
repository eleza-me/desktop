// Plain value types used by generated styles.
#pragma once

#include <QtCore/QMargins>
#include <QtCore/QPoint>
#include <QtCore/QSize>
#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtCore/Qt>

namespace style {

using margins = QMargins;
using size = QSize;
using point = QPoint;
using align = Qt::Alignment;
using cursor = Qt::CursorShape;

// Alignment constants are *logical*: al_left means the leading side in widgets
// that take a style::align (left in LTR, right in RTL).
inline constexpr align al_topleft = Qt::AlignTop | Qt::AlignLeft;
inline constexpr align al_top = Qt::AlignTop | Qt::AlignHCenter;
inline constexpr align al_topright = Qt::AlignTop | Qt::AlignRight;
inline constexpr align al_left = Qt::AlignVCenter | Qt::AlignLeft;
inline constexpr align al_center = Qt::AlignVCenter | Qt::AlignHCenter;
inline constexpr align al_right = Qt::AlignVCenter | Qt::AlignRight;
inline constexpr align al_bottomleft = Qt::AlignBottom | Qt::AlignLeft;
inline constexpr align al_bottom = Qt::AlignBottom | Qt::AlignHCenter;
inline constexpr align al_bottomright = Qt::AlignBottom | Qt::AlignRight;

inline constexpr cursor cur_default = Qt::ArrowCursor;
inline constexpr cursor cur_pointer = Qt::PointingHandCursor;
inline constexpr cursor cur_text = Qt::IBeamCursor;
inline constexpr cursor cur_sizehor = Qt::SizeHorCursor;
inline constexpr cursor cur_sizever = Qt::SizeVerCursor;

// The interface direction (follows the language: Persian = RTL).
[[nodiscard]] bool RightToLeft();
void SetRightToLeft(bool rtl);

// Mirrors a rect inside an outer width when the interface is RTL.
[[nodiscard]] inline QRect rtlrect(int x, int y, int w, int h, int outerw) {
	return QRect(RightToLeft() ? (outerw - x - w) : x, y, w, h);
}
[[nodiscard]] inline QRect rtlrect(const QRect &r, int outerw) {
	return RightToLeft()
		? QRect(outerw - r.x() - r.width(), r.y(), r.width(), r.height())
		: r;
}
[[nodiscard]] inline QPoint rtlpoint(int x, int y, int outerw) {
	return QPoint(RightToLeft() ? (outerw - x) : x, y);
}
[[nodiscard]] inline QPoint rtlpoint(const QPoint &p, int outerw) {
	return RightToLeft() ? QPoint(outerw - p.x(), p.y()) : p;
}
// Swaps left / right of margins when the interface is RTL.
[[nodiscard]] inline QMargins rtlmargins(const QMargins &m) {
	return RightToLeft()
		? QMargins(m.right(), m.top(), m.left(), m.bottom())
		: m;
}

} // namespace style
