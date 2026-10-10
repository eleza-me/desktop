#include "ui/rp_widget.h"

#include <QtCore/QEvent>
#include <QtGui/QPaintEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QMoveEvent>

namespace Ui {

RpWidget::RpWidget(QWidget *parent) : QWidget(parent) {
}

void RpWidget::moveToLeft(int x, int y, int outerw) {
	if (style::RightToLeft()) {
		const auto outer = (outerw > 0)
			? outerw
			: (parentWidget() ? parentWidget()->width() : 0);
		move(outer - x - width(), y);
	} else {
		move(x, y);
	}
}

void RpWidget::moveToRight(int x, int y, int outerw) {
	if (style::RightToLeft()) {
		move(x, y);
	} else {
		const auto outer = (outerw > 0)
			? outerw
			: (parentWidget() ? parentWidget()->width() : 0);
		move(outer - x - width(), y);
	}
}

void RpWidget::setGeometryToLeft(int x, int y, int w, int h, int outerw) {
	if (style::RightToLeft()) {
		const auto outer = (outerw > 0)
			? outerw
			: (parentWidget() ? parentWidget()->width() : 0);
		setGeometry(outer - x - w, y, w, h);
	} else {
		setGeometry(x, y, w, h);
	}
}

void RpWidget::setGeometryToRight(int x, int y, int w, int h, int outerw) {
	if (style::RightToLeft()) {
		setGeometry(x, y, w, h);
	} else {
		const auto outer = (outerw > 0)
			? outerw
			: (parentWidget() ? parentWidget()->width() : 0);
		setGeometry(outer - x - w, y, w, h);
	}
}

void RpWidget::resizeToWidth(int newWidth) {
	const auto newHeight = resizeGetHeight(newWidth);
	const auto newSize = QSize(newWidth, newHeight);
	if (newSize != size()) {
		resize(newSize);
		update();
	}
}

void RpWidget::resizeToNaturalWidth(int newWidth) {
	const auto natural = naturalWidth();
	resizeToWidth((natural >= 0) ? qMin(natural, newWidth) : newWidth);
}

void RpWidget::setVisibleTopBottom(int visibleTop, int visibleBottom) {
	const auto max = height();
	visibleTopBottomUpdated(
		qBound(0, visibleTop, max),
		qBound(0, visibleBottom, max));
}

bool RpWidget::event(QEvent *e) {
	const auto type = e->type();
	if (type == QEvent::Resize) {
		const auto was = static_cast<QResizeEvent*>(e)->oldSize();
		const auto result = QWidget::event(e);
		const auto now = size();
		emit sizeChanged(now);
		if (was.width() != now.width()) {
			emit widthChanged(now.width());
		}
		if (was.height() != now.height()) {
			emit heightChanged(now.height());
		}
		emit geometryChanged(geometry());
		return result;
	} else if (type == QEvent::Move) {
		const auto result = QWidget::event(e);
		emit geometryChanged(geometry());
		return result;
	} else if (type == QEvent::Show || type == QEvent::Hide) {
		const auto result = QWidget::event(e);
		emit shownChanged(type == QEvent::Show);
		return result;
	}
	return QWidget::event(e);
}

void RpWidget::paintEvent(QPaintEvent *e) {
	emit paintRequest(e->rect());
}

} // namespace Ui
