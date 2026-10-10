// Scripted input for automated screenshots and headless interaction tests
// (--debug-actions). The script is a list of steps separated by ";":
//
//   click:x,y          left click at a point of the main window (logical pixels)
//   rclick:x,y         right click (context menus)
//   dclick:x,y         double click
//   move:x,y           move the mouse (hover states, tooltips)
//   press:x,y          press and hold the left button …
//   release:x,y        … and release it (a drag: press, move, move, release)
//   drag:x1,y1,x2,y2   press at the first point, move in steps, release at the second
//   scroll:x,y,dy      wheel at a point; dy > 0 scrolls the content down by dy pixels
//   type:text          types the text into the focused widget ("\;" for a ";")
//   key:Return         a key or a shortcut: Return, Esc, Tab, Backspace, Up, Ctrl+A …
//   paste:text         puts the text on the clipboard and presses Ctrl+V
//   route:chat/c1      opens a route (as --debug-route does)
//   resize:520x800     resizes the window (wide <-> single column)
//   wait:300           waits (milliseconds)
//   grab:/tmp/a.png    saves a grab right now and continues
//
// Mouse steps go to whatever is under the point: the window content, a box, or a
// popup menu / dropdown that opened over the window. Grabs include popups and
// other top-level windows of the app (menus, notification toasts) at their place
// over the main window.
//
// Only in builds with the debug tools (ELEZA_DEBUG_TOOLS).
#pragma once

#ifdef ELEZA_DEBUG_TOOLS

#include <QtCore/QObject>
#include <QtCore/QPoint>
#include <QtCore/QStringList>
#include <QtGui/QImage>

#include <functional>

namespace Window {
class MainWindow;
} // namespace Window

namespace Core {

// The main window with every other visible top-level widget of the app drawn
// over it at its position.
[[nodiscard]] QImage GrabWindowWithPopups(Window::MainWindow *window);

class DebugActions final : public QObject {
public:
	DebugActions(
		Window::MainWindow *window,
		const QString &script,
		std::function<void(const QString &route)> openRoute,
		QObject *parent = nullptr);

	// Plays the steps one after another, then calls `done`.
	void start(std::function<void()> done);

	// "a;b\;c;d" -> { "a", "b;c", "d" }.
	[[nodiscard]] static QStringList Split(const QString &script);

private:
	void next();
	// Returns the delay before the next step.
	[[nodiscard]] int perform(const QString &step);
	void mouse(int type, QPoint position, Qt::MouseButton button, bool down);
	void wheel(QPoint position, int dy);
	void key(const QString &name);
	void type(const QString &text);

	Window::MainWindow *_window = nullptr;
	QStringList _steps;
	int _index = 0;
	bool _leftDown = false;
	int _timestamp = 1000;
	std::function<void(const QString &route)> _openRoute;
	std::function<void()> _done;

};

} // namespace Core

#endif // ELEZA_DEBUG_TOOLS
