#include "ui/ui_utility.h"

#include "ui/rtl.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>
#include <QtGui/QCursor>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>

namespace Ui {
namespace {

bool AnimationsDisabledValue = false;

void SendPendingEventsRecursive(QWidget *target) {
	if (target->testAttribute(Qt::WA_PendingMoveEvent)) {
		auto e = QMoveEvent(target->pos(), QPoint());
		QCoreApplication::sendEvent(target, &e);
		target->setAttribute(Qt::WA_PendingMoveEvent, false);
	}
	if (target->testAttribute(Qt::WA_PendingResizeEvent)) {
		auto e = QResizeEvent(target->size(), QSize());
		QCoreApplication::sendEvent(target, &e);
		target->setAttribute(Qt::WA_PendingResizeEvent, false);
	}
	const auto children = target->children();
	for (const auto child : children) {
		if (child->isWidgetType()) {
			const auto widget = static_cast<QWidget*>(child);
			if (!widget->isWindow()) {
				SendPendingEventsRecursive(widget);
			}
		}
	}
}

} // namespace

qint64 Now() {
	static const auto timer = [] {
		auto result = QElapsedTimer();
		result.start();
		return result;
	}();
	return timer.elapsed();
}

void PostponeCall(QObject *guard, std::function<void()> callback) {
	QTimer::singleShot(0, guard, std::move(callback));
}

void CallDelayed(int ms, QObject *guard, std::function<void()> callback) {
	QTimer::singleShot(ms, guard, std::move(callback));
}

void SendPendingMoveResizeEvents(QWidget *target) {
	SendPendingEventsRecursive(target);
}

QPixmap GrabWidget(QWidget *target, QRect rect, QColor bg) {
	SendPendingMoveResizeEvents(target);
	if (rect.isNull()) {
		rect = target->rect();
	}
	const auto ratio = DevicePixelRatio(target);
	auto result = QPixmap(rect.size() * ratio);
	result.setDevicePixelRatio(ratio);
	result.fill(bg);
	if (!rect.isEmpty()) {
		auto p = QPainter(&result);
		target->render(
			&p,
			QPoint(),
			rect,
			QWidget::DrawChildren | QWidget::IgnoreMask);
	}
	return result;
}

QImage GrabWidgetToImage(QWidget *target, QRect rect, QColor bg) {
	SendPendingMoveResizeEvents(target);
	if (rect.isNull()) {
		rect = target->rect();
	}
	const auto ratio = DevicePixelRatio(target);
	auto result = QImage(
		rect.size() * ratio,
		QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(ratio);
	result.fill(bg);
	if (!rect.isEmpty()) {
		auto p = QPainter(&result);
		target->render(
			&p,
			QPoint(),
			rect,
			QWidget::DrawChildren | QWidget::IgnoreMask);
	}
	return result;
}

bool IsAncestorOf(const QWidget *parent, const QWidget *child) {
	while (child) {
		if (child == parent) {
			return true;
		}
		child = child->parentWidget();
	}
	return false;
}

double DevicePixelRatio(const QWidget *widget) {
	return widget ? widget->devicePixelRatioF() : qApp->devicePixelRatio();
}

void SendSynteticMouseEvent(QWidget *widget) {
	if (!widget || !widget->isVisible()) {
		return;
	}
	const auto global = QCursor::pos();
	const auto local = widget->mapFromGlobal(global);
	auto e = QMouseEvent(
		QEvent::MouseMove,
		QPointF(local),
		QPointF(global),
		Qt::NoButton,
		QApplication::mouseButtons(),
		QApplication::keyboardModifiers());
	QCoreApplication::sendEvent(widget, &e);
}

void SetAnimationsDisabled(bool disabled) {
	AnimationsDisabledValue = disabled;
}

bool AnimationsDisabled() {
	return AnimationsDisabledValue;
}

bool IsRtlText(const QString &text) {
	for (const auto ch : text) {
		switch (ch.direction()) {
		case QChar::DirL: return false;
		case QChar::DirR:
		case QChar::DirAL: return true;
		default: break;
		}
	}
	return style::RightToLeft();
}

} // namespace Ui
