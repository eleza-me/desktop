// Small helpers used all over the UI code.
#pragma once

#include <QtCore/QPointer>
#include <QtCore/QRect>
#include <QtGui/QImage>
#include <QtGui/QPixmap>
#include <QtWidgets/QWidget>

#include <functional>

namespace Ui {

// Monotonic milliseconds since the app start.
[[nodiscard]] qint64 Now();

// Runs `callback` on the next loop iteration while `guard` is alive.
void PostponeCall(QObject *guard, std::function<void()> callback);

// Calls `callback` once after `ms` while `guard` is alive.
void CallDelayed(int ms, QObject *guard, std::function<void()> callback);

// Renders a widget (with children) to a pixmap at its device pixel ratio.
// `rect` in widget coordinates, the whole widget if null. `bg` fills first.
[[nodiscard]] QPixmap GrabWidget(
	QWidget *target,
	QRect rect = QRect(),
	QColor bg = QColor(255, 255, 255, 0));
[[nodiscard]] QImage GrabWidgetToImage(
	QWidget *target,
	QRect rect = QRect(),
	QColor bg = QColor(255, 255, 255, 0));

// Makes sure all pending layout (resize / show) events of the tree are delivered,
// so a grab made right after creation is not empty.
void SendPendingMoveResizeEvents(QWidget *target);

// True if `child` is `parent` or inside it.
[[nodiscard]] bool IsAncestorOf(const QWidget *parent, const QWidget *child);

// The device pixel ratio the widget is painted with.
[[nodiscard]] double DevicePixelRatio(const QWidget *widget);

// Re-sends a synthetic mouse move so hover states update after layout changes.
void SendSynteticMouseEvent(QWidget *widget);

// Marks a widget as not taking part in mouse handling.
inline void MakeTransparentForMouse(QWidget *widget, bool transparent = true) {
	widget->setAttribute(Qt::WA_TransparentForMouseEvents, transparent);
}

// Disables / enables every animation (frames jump to the end). Used by tests
// and by the reduced motion setting.
void SetAnimationsDisabled(bool disabled);
[[nodiscard]] bool AnimationsDisabled();

} // namespace Ui
