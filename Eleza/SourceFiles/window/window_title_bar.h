// Window::TitleBar — the title bar the app paints itself: a slim strip at the
// top of the main window with the title and the three window buttons
// (minimize, maximize / restore, close).
//
// The widget is platform neutral. It paints, keeps the geometry and the state
// of the buttons and reports what the user asked for; the platform part of the
// frame (window_title_frame.h) does the rest: on Windows it answers the system
// hit-test from buttonRect() / layout() and feeds the maximize button through
// setNativeOver() / setNativePressed() / nativeClick(), because the system
// keeps the mouse over that button to itself; elsewhere the bar moves the
// window by itself (a drag starts a system move, a double click maximizes).
//
// Layout (window_title_hit_test.h, unit-tested): the buttons sit at the
// physical top right corner in every interface language — that is where the
// platform has them — and the title text follows the interface direction.
//
// Overlay policy. The bar is not part of the window body: everything that
// covers "the whole window" (boxes and their dimming, toasts, the image
// viewer, the video pager, the intro, the "update required" screen) covers the
// body below the bar, and the bar with its buttons stays reachable all the
// time. For the dark full-window media overlays the bar switches to a dark
// variant while they are shown (MainWindow::registerDarkOverlay), so the
// picture is not framed by a light strip. In fullscreen the bar is hidden.
#pragma once

#include "ui/rp_widget.h"
#include "window/window_title_hit_test.h"

#include <QtCore/QTimer>

namespace Window {

class TitleBar final : public Ui::RpWidget {
	Q_OBJECT

public:
	enum class Button {
		None,
		Minimize,
		Maximize,
		Close,
	};

	explicit TitleBar(QWidget *parent);
	~TitleBar();

	void setTitle(const QString &title);
	// Inactive window: the title and the buttons are dimmed.
	void setActive(bool active);
	// Maximized window: the middle button restores.
	void setMaximized(bool maximized);
	[[nodiscard]] bool maximized() const {
		return _maximized;
	}
	// The dark variant, while a dark media overlay covers the body.
	void setDarkOverlay(bool dark);
	// Part of the bar at the leading side that continues into something of
	// the same surface below it (the nav rail): no separator line there.
	void setJoinedWidth(int width);

	// ---- geometry, in the bar's own coordinates (logical pixels).
	[[nodiscard]] TitleHit::BarLayout layout() const;
	[[nodiscard]] QRect buttonRect(Button button) const;
	// The part the window is dragged by.
	[[nodiscard]] QRect draggableRect() const;
	[[nodiscard]] Button buttonAt(QPoint position) const;

	// ---- for a frame that owns the mouse over a button.
	void setNativeOver(Button button, bool withTooltip = true);
	void setNativePressed(Button button);
	[[nodiscard]] Button nativePressed() const {
		return _nativeDown;
	}
	// A click the frame saw: the same as one made on the widget.
	void nativeClick(Button button);
	// Forgets hover and press (the window was deactivated, hidden, moved).
	void clearState();

	// The color of the bar right now (the frame fills what is not painted
	// yet with it and tints the system border).
	[[nodiscard]] QColor backgroundColor() const;
	[[nodiscard]] QColor borderColor() const;

Q_SIGNALS:
	void minimizeRequested();
	// Maximize, or restore when maximized.
	void maximizeToggleRequested();
	void closeRequested();

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void hideEvent(QHideEvent *e) override;

private:
	[[nodiscard]] Button overButton() const {
		return (_nativeOver != Button::None) ? _nativeOver : _over;
	}
	[[nodiscard]] Button downButton() const {
		return (_nativeDown != Button::None) ? _nativeDown : _down;
	}
	void setOver(Button button);
	void overChanged(Button was, bool withTooltip);
	void activate(Button button);
	void updateButton(Button button);
	void showTooltip();
	void hideTooltip();
	[[nodiscard]] QString tooltipText(Button button) const;
	void paintButton(QPainter &p, Button button) const;
	void paintGlyph(
		QPainter &p,
		Button button,
		const QRect &rect,
		const QColor &color) const;

	QString _title;
	bool _active = true;
	bool _maximized = false;
	bool _dark = false;
	int _joinedWidth = 0;

	Button _over = Button::None;
	Button _down = Button::None;
	Button _nativeOver = Button::None;
	Button _nativeDown = Button::None;
	bool _dragPressed = false;
	QPoint _dragStart;

	QTimer _tooltipTimer;
	bool _tooltipShown = false;

};

} // namespace Window
