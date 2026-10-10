// The platform half of the custom window frame (window_title_bar.h is the
// widget half).
//
//   Windows   window_title_win.cpp: the window keeps its native styles, so the
//             shadow, snapping, the animations and the taskbar behave as for
//             any other window; the non-client area is removed and the system
//             hit-test is answered from the bar's geometry.
//   Linux     window_title_frame.cpp: a frameless window with an invisible
//             resize border; the bar moves the window through the compositor.
//
// Whether the app draws its own title bar is decided once per run
// (UseCustomTitleBar): the "use system title bar" preference, the command line
// (--system-title-bar) and the environment (ELEZA_SYSTEM_TITLE_BAR=1) turn it
// off; a platform the frame was not written for never has it.
#pragma once

#include <QtCore/QtGlobal>

#include <functional>
#include <memory>

class QWidget;

namespace Window {

class TitleBar;

class TitleFrame {
public:
	virtual ~TitleFrame() = default;

	// The window was resized, or maximized / restored / put in fullscreen.
	virtual void windowChanged() {
	}
	// Light <-> dark, or the bar changed its variant.
	virtual void paletteChanged() {
	}
	// The native window behind the widget was created anew.
	virtual void nativeWindowChanged() {
	}
	// From QWidget::nativeEvent of the window.
	[[nodiscard]] virtual bool nativeEvent(void *message, qintptr *result) {
		Q_UNUSED(message);
		Q_UNUSED(result);
		return false;
	}
};

// The platform (and the Qt platform plugin of this run) has a custom frame;
// the preference is offered in the settings only then.
[[nodiscard]] bool CustomTitleBarSupported();
// This run draws its own title bar.
[[nodiscard]] bool UseCustomTitleBar();

// Takes the system frame away from `window` (before it is shown) and wires
// the bar to the window system. Null when it could not be done: the caller
// goes on with the system frame then, unless it shows the bar anyway for a
// screenshot. `lost` is called if the frame has to give the window back to
// the system later (the bar must go then).
[[nodiscard]] std::unique_ptr<TitleFrame> CreateTitleFrame(
	QWidget *window,
	TitleBar *title,
	std::function<void()> lost);

} // namespace Window
