// Painter: QPainter with RTL-aware helpers. "Left" always means the leading side
// (left in LTR, right in RTL) inside `outerw`.
#pragma once

#include "ui/style/style_core.h"
#include "ui/rtl.h"

#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

class Painter : public QPainter {
public:
	explicit Painter(QPaintDevice *device) : QPainter(device) {
		setLayoutDirection(Ui::LayoutDirection());
	}

	// `y` is the top of the text line, not the baseline.
	void drawTextLeft(
			int x,
			int y,
			int outerw,
			const QString &text,
			int textWidth = -1) {
		const auto metrics = fontMetrics();
		if (style::RightToLeft() && textWidth < 0) {
			textWidth = metrics.horizontalAdvance(text);
		}
		drawText(
			style::RightToLeft() ? (outerw - x - textWidth) : x,
			y + metrics.ascent(),
			text);
	}
	void drawTextRight(
			int x,
			int y,
			int outerw,
			const QString &text,
			int textWidth = -1) {
		const auto metrics = fontMetrics();
		if (!style::RightToLeft() && textWidth < 0) {
			textWidth = metrics.horizontalAdvance(text);
		}
		drawText(
			style::RightToLeft() ? x : (outerw - x - textWidth),
			y + metrics.ascent(),
			text);
	}
	void drawTextCentered(const QRect &rect, const QString &text) {
		const auto metrics = fontMetrics();
		const auto width = metrics.horizontalAdvance(text);
		drawText(
			rect.x() + (rect.width() - width) / 2,
			rect.y() + (rect.height() - metrics.height()) / 2 + metrics.ascent(),
			text);
	}

	void drawPixmapLeft(int x, int y, int outerw, const QPixmap &pixmap) {
		const auto width = int(pixmap.width() / pixmap.devicePixelRatio());
		drawPixmap(
			QPoint(style::RightToLeft() ? (outerw - x - width) : x, y),
			pixmap);
	}
	void drawPixmapRight(int x, int y, int outerw, const QPixmap &pixmap) {
		const auto width = int(pixmap.width() / pixmap.devicePixelRatio());
		drawPixmap(
			QPoint(style::RightToLeft() ? x : (outerw - x - width), y),
			pixmap);
	}

	void fillRectLeft(int x, int y, int w, int h, int outerw, const QBrush &brush) {
		fillRect(style::rtlrect(x, y, w, h, outerw), brush);
	}

};

// Antialiasing for the lifetime of the object.
class PainterHighQualityEnabler {
public:
	explicit PainterHighQualityEnabler(QPainter &p) : _p(p) {
		const auto wanted = QPainter::Antialiasing
			| QPainter::SmoothPixmapTransform
			| QPainter::TextAntialiasing;
		_was = _p.renderHints();
		_p.setRenderHints(wanted, true);
	}
	~PainterHighQualityEnabler() {
		_p.setRenderHints(
			QPainter::Antialiasing
			| QPainter::SmoothPixmapTransform
			| QPainter::TextAntialiasing,
			false);
		_p.setRenderHints(_was, true);
	}

	PainterHighQualityEnabler(const PainterHighQualityEnabler &) = delete;
	PainterHighQualityEnabler &operator=(
		const PainterHighQualityEnabler &) = delete;

private:
	QPainter &_p;
	QPainter::RenderHints _was;

};
