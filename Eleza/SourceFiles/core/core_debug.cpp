#include "core/core_debug.h"

#include "ui/ui_utility.h"
#include "window/window_main_window.h"

#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QWheelEvent>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>

// The entry points Qt's own test library uses to inject input the way a platform
// plugin delivers it (shortcut map, popups, enter / leave, double clicks). They
// are exported by QtGui; declaring them here avoids linking QtTest into the app.
Q_GUI_EXPORT void qt_handleMouseEvent(
	QWindow *window,
	const QPointF &local,
	const QPointF &global,
	Qt::MouseButtons state,
	Qt::MouseButton button,
	QEvent::Type type,
	Qt::KeyboardModifiers mods,
	int timestamp);
Q_GUI_EXPORT bool qt_handleKeyEvent(
	QWindow *window,
	QEvent::Type t,
	int k,
	Qt::KeyboardModifiers mods,
	const QString &text,
	bool autorep,
	ushort count);

namespace Core {
namespace {

constexpr auto kStepDelay = 80;
constexpr auto kDragSteps = 6;

[[nodiscard]] QPoint GlobalOrigin(QWidget *widget) {
	return widget->mapToGlobal(QPoint());
}

// Top-level widgets shown over the main window (menus, notifications), in
// stacking order as far as Qt tells it.
[[nodiscard]] QList<QWidget*> OtherTopLevels(QWidget *window) {
	auto result = QList<QWidget*>();
	const auto list = QApplication::topLevelWidgets();
	for (const auto widget : list) {
		if (widget != window
			&& widget->isVisible()
			&& !widget->size().isEmpty()) {
			result.push_back(widget);
		}
	}
	return result;
}

[[nodiscard]] bool ParsePoint(const QStringList &parts, int from, QPoint &to) {
	if (parts.size() < from + 2) {
		return false;
	}
	auto okX = false;
	auto okY = false;
	const auto x = parts[from].trimmed().toInt(&okX);
	const auto y = parts[from + 1].trimmed().toInt(&okY);
	if (!okX || !okY) {
		return false;
	}
	to = QPoint(x, y);
	return true;
}

} // namespace

QImage GrabWindowWithPopups(Window::MainWindow *window) {
	auto result = window->grabForDebug();
	const auto ratio = result.devicePixelRatio();
	const auto origin = GlobalOrigin(window);
	auto p = QPainter(&result);
	for (const auto widget : OtherTopLevels(window)) {
		Ui::SendPendingMoveResizeEvents(widget);
		auto image = Ui::GrabWidgetToImage(widget);
		image.setDevicePixelRatio(ratio);
		p.drawImage(GlobalOrigin(widget) - origin, image);
	}
	return result;
}

DebugActions::DebugActions(
	Window::MainWindow *window,
	const QString &script,
	std::function<void(const QString &route)> openRoute,
	QObject *parent)
: QObject(parent)
, _window(window)
, _steps(Split(script))
, _openRoute(std::move(openRoute)) {
}

QStringList DebugActions::Split(const QString &script) {
	auto result = QStringList();
	auto current = QString();
	for (auto i = 0; i != script.size(); ++i) {
		const auto ch = script[i];
		if (ch == QLatin1Char('\\')
			&& i + 1 < script.size()
			&& script[i + 1] == QLatin1Char(';')) {
			current.append(QLatin1Char(';'));
			++i;
		} else if (ch == QLatin1Char(';')) {
			if (!current.trimmed().isEmpty()) {
				result.push_back(current);
			}
			current.clear();
		} else {
			current.append(ch);
		}
	}
	if (!current.trimmed().isEmpty()) {
		result.push_back(current);
	}
	return result;
}

void DebugActions::start(std::function<void()> done) {
	_done = std::move(done);
	_index = 0;
	QTimer::singleShot(0, this, [this] { next(); });
}

void DebugActions::next() {
	if (_index >= _steps.size()) {
		if (const auto done = std::exchange(_done, nullptr)) {
			done();
		}
		return;
	}
	const auto delay = perform(_steps[_index++]);
	QTimer::singleShot(delay, this, [this] { next(); });
}

int DebugActions::perform(const QString &step) {
	const auto colon = step.indexOf(QLatin1Char(':'));
	const auto name = ((colon < 0) ? step : step.left(colon)).trimmed().toLower();
	const auto value = (colon < 0) ? QString() : step.mid(colon + 1);
	const auto parts = value.split(QLatin1Char(','));
	auto point = QPoint();
	const auto bad = [&] {
		qWarning("debug-actions: cannot run the step '%s'.", qPrintable(step));
		return kStepDelay;
	};
	if (name == QStringLiteral("wait")) {
		return std::max(value.trimmed().toInt(), 0);
	} else if (name == QStringLiteral("grab")) {
		const auto path = value.trimmed();
		if (!GrabWindowWithPopups(_window).save(path)) {
			qWarning("debug-actions: could not save '%s'.", qPrintable(path));
		}
	} else if (name == QStringLiteral("type")) {
		type(value);
	} else if (name == QStringLiteral("key")) {
		key(value.trimmed());
	} else if (name == QStringLiteral("paste")) {
		QGuiApplication::clipboard()->setText(value);
		key(QStringLiteral("Ctrl+V"));
	} else if (name == QStringLiteral("route")) {
		if (_openRoute) {
			_openRoute(value.trimmed());
		}
	} else if (name == QStringLiteral("click")
		|| name == QStringLiteral("rclick")
		|| name == QStringLiteral("dclick")) {
		if (!ParsePoint(parts, 0, point)) {
			return bad();
		}
		const auto button = (name == QStringLiteral("rclick"))
			? Qt::RightButton
			: Qt::LeftButton;
		mouse(QEvent::MouseMove, point, Qt::NoButton, false);
		mouse(QEvent::MouseButtonPress, point, button, true);
		mouse(QEvent::MouseButtonRelease, point, button, false);
		if (name == QStringLiteral("dclick")) {
			// Qt turns the second press into a double click by itself.
			mouse(QEvent::MouseButtonPress, point, button, true);
			mouse(QEvent::MouseButtonRelease, point, button, false);
		} else {
			// Keep separate clicks from merging into a double click.
			_timestamp += 1000;
		}
	} else if (name == QStringLiteral("move")) {
		if (!ParsePoint(parts, 0, point)) {
			return bad();
		}
		mouse(QEvent::MouseMove, point, Qt::NoButton, _leftDown);
	} else if (name == QStringLiteral("press")) {
		if (!ParsePoint(parts, 0, point)) {
			return bad();
		}
		mouse(QEvent::MouseMove, point, Qt::NoButton, false);
		mouse(QEvent::MouseButtonPress, point, Qt::LeftButton, true);
		_leftDown = true;
	} else if (name == QStringLiteral("release")) {
		if (!ParsePoint(parts, 0, point)) {
			return bad();
		}
		mouse(QEvent::MouseButtonRelease, point, Qt::LeftButton, false);
		_leftDown = false;
	} else if (name == QStringLiteral("drag")) {
		auto to = QPoint();
		if (!ParsePoint(parts, 0, point) || !ParsePoint(parts, 2, to)) {
			return bad();
		}
		mouse(QEvent::MouseMove, point, Qt::NoButton, false);
		mouse(QEvent::MouseButtonPress, point, Qt::LeftButton, true);
		for (auto i = 1; i <= kDragSteps; ++i) {
			const auto now = point + (to - point) * i / kDragSteps;
			mouse(QEvent::MouseMove, now, Qt::NoButton, true);
		}
		mouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, false);
	} else if (name == QStringLiteral("scroll")) {
		if (!ParsePoint(parts, 0, point) || parts.size() < 3) {
			return bad();
		}
		mouse(QEvent::MouseMove, point, Qt::NoButton, false);
		wheel(point, parts[2].trimmed().toInt());
	} else {
		return bad();
	}
	return kStepDelay;
}

void DebugActions::mouse(
		int type,
		QPoint position,
		Qt::MouseButton button,
		bool down) {
	const auto global = GlobalOrigin(_window) + position;
	// A popup under the point gets the event, otherwise the main window (Qt
	// itself redirects to an active popup that wants outside clicks).
	auto target = static_cast<QWidget*>(_window);
	for (const auto widget : OtherTopLevels(_window)) {
		if (QRect(GlobalOrigin(widget), widget->size()).contains(global)) {
			target = widget;
		}
	}
	const auto handle = target->windowHandle();
	if (!handle) {
		return;
	}
	const auto heldButton = (button == Qt::NoButton) ? Qt::LeftButton : button;
	const auto buttons = down
		? Qt::MouseButtons(heldButton)
		: Qt::MouseButtons(Qt::NoButton);
	_timestamp += 20;
	qt_handleMouseEvent(
		handle,
		QPointF(global - GlobalOrigin(target)),
		QPointF(global),
		buttons,
		button,
		QEvent::Type(type),
		Qt::NoModifier,
		_timestamp);
}

void DebugActions::wheel(QPoint position, int dy) {
	const auto global = GlobalOrigin(_window) + position;
	// A point outside of the (offscreen) screen has no widget for Qt: ask the
	// window itself then.
	auto under = QApplication::widgetAt(global);
	if (!under) {
		const auto child = _window->childAt(position);
		under = child ? child : static_cast<QWidget*>(_window);
	}
	// Qt does not propagate a synthesized wheel event to the parents by
	// itself: walk up from the widget under the point until one takes it.
	for (auto receiver = under
		; receiver
		; receiver = receiver->isWindow() ? nullptr : receiver->parentWidget()) {
		// Moving the content down by dy pixels = a negative wheel delta.
		auto event = QWheelEvent(
			QPointF(receiver->mapFromGlobal(global)),
			QPointF(global),
			QPoint(0, -dy),
			QPoint(0, -dy),
			Qt::NoButton,
			Qt::NoModifier,
			Qt::ScrollUpdate,
			false);
		event.ignore();
		QCoreApplication::sendEvent(receiver, &event);
		if (event.isAccepted()) {
			break;
		}
	}
}

void DebugActions::key(const QString &name) {
	const auto sequence = QKeySequence::fromString(name, QKeySequence::PortableText);
	if (sequence.isEmpty()) {
		qWarning("debug-actions: unknown key '%s'.", qPrintable(name));
		return;
	}
	const auto combination = sequence[0];
	const auto key = combination.key();
	const auto modifiers = combination.keyboardModifiers();
	auto text = QString();
	if (key == Qt::Key_Return || key == Qt::Key_Enter) {
		text = QStringLiteral("\r");
	} else if (key == Qt::Key_Space) {
		text = QStringLiteral(" ");
	} else if (key >= 0x20 && key < 0x7F && !(modifiers & ~Qt::ShiftModifier)) {
		text = QString(QChar(ushort(key)));
		if (!(modifiers & Qt::ShiftModifier)) {
			text = text.toLower();
		}
	}
	const auto receiver = QGuiApplication::focusWindow()
		? QGuiApplication::focusWindow()
		: _window->windowHandle();
	if (!receiver) {
		return;
	}
	qt_handleKeyEvent(receiver, QEvent::KeyPress, key, modifiers, text, false, 1);
	qt_handleKeyEvent(receiver, QEvent::KeyRelease, key, modifiers, text, false, 1);
}

void DebugActions::type(const QString &text) {
	auto i = 0;
	while (i < text.size()) {
		// Keep surrogate pairs together.
		const auto length = (text[i].isHighSurrogate() && i + 1 < text.size())
			? 2
			: 1;
		const auto piece = text.mid(i, length);
		i += length;
		const auto receiver = QGuiApplication::focusWindow()
			? QGuiApplication::focusWindow()
			: _window->windowHandle();
		if (!receiver) {
			return;
		}
		const auto ch = piece[0];
		const auto key = (length == 1 && ch.unicode() >= 0x20 && ch.unicode() < 0x7F)
			? int(ch.toUpper().unicode())
			: int(Qt::Key_unknown);
		qt_handleKeyEvent(receiver, QEvent::KeyPress, key, Qt::NoModifier, piece, false, 1);
		qt_handleKeyEvent(receiver, QEvent::KeyRelease, key, Qt::NoModifier, piece, false, 1);
	}
}

} // namespace Core
