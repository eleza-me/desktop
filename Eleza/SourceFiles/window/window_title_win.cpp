// The custom window frame on Windows (window_title_frame.h). Compiled on
// Windows only.
//
// The window keeps the styles of an ordinary one (WS_THICKFRAME, WS_CAPTION,
// the minimize / maximize boxes, WS_SYSMENU), so the shadow, snapping by drag
// and by the keyboard, the minimize / maximize animations and the taskbar
// treat it as any other window. What changes:
//
//  * WM_NCCALCSIZE: there is no non-client area — the client rectangle is the
//    window rectangle. A maximized window hangs over its monitor by the frame
//    thickness, so there the client rectangle is cut back to the monitor (and
//    one pixel more on an edge that holds an auto-hide taskbar).
//  * WM_NCHITTEST is answered from the geometry of Window::TitleBar
//    (window_title_hit_test.h): resize borders, the draggable part of the bar
//    (HTCAPTION: move, double click, snapping), the maximize button
//    (HTMAXBUTTON, which also gives the snap layouts of Windows 11). The
//    minimize and close buttons are plain client area: the widget handles the
//    mouse there. Over HTMAXBUTTON the widget gets no mouse events at all, so
//    the non-client mouse messages are translated for it here.
//  * The system menu: a right click on the bar shows it from here (the
//    platform plugin turns a right click inside the client rectangle into a
//    context menu event for the widgets and never reaches the default
//    handler). Alt+Space is the plugin's own.
//  * Nothing of the classic caption is ever painted: WM_NCACTIVATE goes to
//    the default handler with lParam -1, the two undocumented "draw caption /
//    frame" messages are swallowed, WM_NCPAINT is swallowed when there is no
//    composition.
//  * DWM: the frame is extended into the client area by one pixel at the
//    bottom (without it a window that has no non-client area gets no shadow),
//    rounded corners and our border color where the system has them
//    (Windows 11), the dark mode flag follows the palette.
//
// How Qt keeps up. The platform plugin compares the rectangle before and after
// every WM_NCCALCSIZE and takes the difference as the frame margins, including
// when the message was answered by somebody else — so it sees zero margins for
// the normal window and the cut-back for the maximized one, and geometry(),
// setGeometry(), the minimum size (WM_GETMINMAXINFO) and the size after a DPI
// change all come out right without further help. This is the behaviour of
// Qt 6.7 and later (CustomTitleBarSupported() checks the version); should a
// later version disagree, the check in verifyGeometry() notices that Qt and
// the system see different client sizes and gives the system frame back.
//
// The saved window geometry is the window rectangle in both worlds: the frame
// is taken away before the saved geometry is applied, and taking it away keeps
// the client rectangle where it was, so nothing drifts from launch to launch.
//
// Every call into the system here may fail (an old build of Windows, no
// composition): failures are ignored and leave a plainer, working window.
#include "window/window_title_frame.h"

#include "window/window_title_bar.h"
#include "window/window_title_hit_test.h"
#include "ui/style/style_core.h"
#include "styles/style_window.h"

#include <QtCore/QOperatingSystemVersion>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtWidgets/QWidget>

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif // !WM_DPICHANGED

#ifndef WM_DWMCOMPOSITIONCHANGED
#define WM_DWMCOMPOSITIONCHANGED 0x031E
#endif // !WM_DWMCOMPOSITIONCHANGED

#ifndef ABM_GETAUTOHIDEBAREX
#define ABM_GETAUTOHIDEBAREX 0x0000000b
#endif // !ABM_GETAUTOHIDEBAREX

#ifndef USER_DEFAULT_SCREEN_DPI
#define USER_DEFAULT_SCREEN_DPI 96
#endif // !USER_DEFAULT_SCREEN_DPI

namespace Window {
namespace {

using Button = TitleBar::Button;

// Undocumented: sent to paint the classic caption / frame.
constexpr auto kWmNcUahDrawCaption = UINT(0x00AE);
constexpr auto kWmNcUahDrawFrame = UINT(0x00AF);

// DWMWINDOWATTRIBUTE values that an older SDK does not name.
constexpr auto kDwmwaUseImmersiveDarkModeOld = DWORD(19);
constexpr auto kDwmwaUseImmersiveDarkMode = DWORD(20);
constexpr auto kDwmwaWindowCornerPreference = DWORD(33);
constexpr auto kDwmwaBorderColor = DWORD(34);
constexpr auto kDwmwcpRound = DWORD(2);

constexpr auto kCustomFrameStyles = LONG_PTR(WS_CAPTION | WS_THICKFRAME);
// Qt and the system may round a size differently by a pixel.
constexpr auto kGeometryTolerance = 2;
constexpr auto kVerifyRetryDelay = 300;

[[nodiscard]] TitleHit::Rect FromRECT(const RECT &rect) {
	return { rect.left, rect.top, rect.right, rect.bottom };
}

[[nodiscard]] RECT ToRECT(const TitleHit::Rect &rect) {
	auto result = RECT();
	result.left = rect.left;
	result.top = rect.top;
	result.right = rect.right;
	result.bottom = rect.bottom;
	return result;
}

[[nodiscard]] COLORREF ToColorRef(const QColor &color) {
	return RGB(color.red(), color.green(), color.blue());
}

[[nodiscard]] UINT DpiFor(HWND hwnd) {
	const auto dpi = GetDpiForWindow(hwnd);
	return (dpi > 0) ? dpi : UINT(USER_DEFAULT_SCREEN_DPI);
}

[[nodiscard]] int MetricFor(int index, UINT dpi) {
	const auto value = GetSystemMetricsForDpi(index, dpi);
	return (value > 0) ? value : GetSystemMetrics(index);
}

[[nodiscard]] TitleHit::AutoHideEdges AutoHideEdgesFor(HWND hwnd) {
	auto result = TitleHit::AutoHideEdges();
	auto state = APPBARDATA();
	state.cbSize = sizeof(state);
	if (!(SHAppBarMessage(ABM_GETSTATE, &state) & ABS_AUTOHIDE)) {
		return result;
	}
	const auto monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	auto info = MONITORINFO();
	info.cbSize = sizeof(info);
	if (!monitor || !GetMonitorInfoW(monitor, &info)) {
		return result;
	}
	const auto has = [&](UINT edge) {
		auto data = APPBARDATA();
		data.cbSize = sizeof(data);
		data.uEdge = edge;
		data.rc = info.rcMonitor;
		return SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &data) != 0;
	};
	result.left = has(ABE_LEFT);
	result.top = has(ABE_TOP);
	result.right = has(ABE_RIGHT);
	result.bottom = has(ABE_BOTTOM);
	return result;
}

[[nodiscard]] LRESULT HitCode(TitleHit::Zone zone) {
	using Zone = TitleHit::Zone;
	switch (zone) {
	case Zone::Outside: return HTNOWHERE;
	case Zone::Caption: return HTCAPTION;
	case Zone::Maximize: return HTMAXBUTTON;
	case Zone::Left: return HTLEFT;
	case Zone::Top: return HTTOP;
	case Zone::Right: return HTRIGHT;
	case Zone::Bottom: return HTBOTTOM;
	case Zone::TopLeft: return HTTOPLEFT;
	case Zone::TopRight: return HTTOPRIGHT;
	case Zone::BottomLeft: return HTBOTTOMLEFT;
	case Zone::BottomRight: return HTBOTTOMRIGHT;
	// The widget itself handles the mouse over these two.
	case Zone::Minimize:
	case Zone::Close:
	case Zone::Client: break;
	}
	return HTCLIENT;
}

class WinTitleFrame final : public TitleFrame {
public:
	WinTitleFrame(
		QWidget *window,
		TitleBar *title,
		std::function<void()> lost);
	~WinTitleFrame();

	[[nodiscard]] bool installed() const {
		return _installed;
	}

	void windowChanged() override;
	void paletteChanged() override;
	void nativeWindowChanged() override;
	[[nodiscard]] bool nativeEvent(void *message, qintptr *result) override;

private:
	void install();
	void uninstall();
	void applyDwm();
	void refreshBrush();
	void recalculateFrame(bool keepClientRect);
	void scheduleRefresh();
	void scheduleVerify(int delay);
	void verifyGeometry();

	// The window has the frame styles right now (not while Qt has it in
	// fullscreen with the styles of a popup).
	[[nodiscard]] bool hasFrameStyles() const;
	[[nodiscard]] TitleHit::Zone zoneAt(POINT screen) const;
	[[nodiscard]] bool handleCalcSize(MSG *msg, qintptr *result);
	[[nodiscard]] bool handleHitTest(MSG *msg, qintptr *result);
	[[nodiscard]] bool handleNcMouse(MSG *msg, qintptr *result);
	void trackNonClientLeave();
	void clearNativeMouse();
	void showSystemMenu(POINT screen);

	QPointer<QWidget> _window;
	QPointer<TitleBar> _title;
	std::function<void()> _lost;
	// Delayed calls die with it.
	QObject _guard;
	HWND _hwnd = nullptr;
	HBRUSH _brush = nullptr;
	COLORREF _brushColor = 0;
	TitleHit::AutoHideEdges _autoHide;
	bool _installed = false;
	bool _refreshScheduled = false;
	bool _verified = false;
	int _verifyFailures = 0;
	// Windows 11 shows the snap layouts over the maximize button.
	bool _snapLayouts = false;

};

WinTitleFrame::WinTitleFrame(
	QWidget *window,
	TitleBar *title,
	std::function<void()> lost)
: _window(window)
, _title(title)
, _lost(std::move(lost))
, _snapLayouts(QOperatingSystemVersion::current()
	>= QOperatingSystemVersion::Windows11) {
	install();
}

WinTitleFrame::~WinTitleFrame() {
	if (_brush) {
		DeleteObject(_brush);
	}
}

void WinTitleFrame::install() {
	_installed = false;
	_hwnd = nullptr;
	_verified = false;
	_verifyFailures = 0;
	if (!_window || !_title) {
		return;
	}
	// Creates the native window when it does not exist yet.
	const auto hwnd = reinterpret_cast<HWND>(_window->winId());
	if (!hwnd || !IsWindow(hwnd)) {
		return;
	}
	_hwnd = hwnd;
	if (!hasFrameStyles()) {
		// Not a window with a system frame: nothing to take over.
		_hwnd = nullptr;
		return;
	}
	_installed = true;
	refreshBrush();
	applyDwm();
	recalculateFrame(true);
}

void WinTitleFrame::uninstall() {
	if (!_installed) {
		return;
	}
	_installed = false;
	clearNativeMouse();
	if (_hwnd && IsWindow(_hwnd)) {
		const auto margins = MARGINS{ 0, 0, 0, 0 };
		DwmExtendFrameIntoClientArea(_hwnd, &margins);
		// The system frame comes back around the same window rectangle.
		SetWindowPos(
			_hwnd,
			nullptr,
			0,
			0,
			0,
			0,
			(SWP_FRAMECHANGED
				| SWP_NOMOVE
				| SWP_NOSIZE
				| SWP_NOZORDER
				| SWP_NOOWNERZORDER
				| SWP_NOACTIVATE));
	}
	if (const auto lost = _lost) {
		// The window lays itself out without the bar.
		lost();
	}
}

bool WinTitleFrame::hasFrameStyles() const {
	if (!_hwnd) {
		return false;
	}
	const auto style = GetWindowLongPtrW(_hwnd, GWL_STYLE);
	return (style & kCustomFrameStyles) == kCustomFrameStyles;
}

void WinTitleFrame::refreshBrush() {
	// What shows through before the widgets have painted a part of the
	// window that just appeared (growing it, restoring it).
	const auto color = ToColorRef(st::bgCanvas->c);
	if (_brush && _brushColor == color) {
		return;
	}
	if (_brush) {
		DeleteObject(_brush);
	}
	_brush = CreateSolidBrush(color);
	_brushColor = color;
}

void WinTitleFrame::applyDwm() {
	if (!_installed || !_hwnd) {
		return;
	}
	// One pixel of the frame under the bottom edge keeps the shadow. The
	// widgets paint over it; at the bottom nothing is seen of it even
	// before they did.
	const auto margins = MARGINS{ 0, 0, 0, 1 };
	DwmExtendFrameIntoClientArea(_hwnd, &margins);

	// Rounded corners and the border color: Windows 11, an error elsewhere.
	const auto corners = kDwmwcpRound;
	DwmSetWindowAttribute(
		_hwnd,
		kDwmwaWindowCornerPreference,
		&corners,
		sizeof(corners));
	if (_title) {
		const auto border = ToColorRef(_title->borderColor());
		DwmSetWindowAttribute(
			_hwnd,
			kDwmwaBorderColor,
			&border,
			sizeof(border));
	}

	// The system's own parts of the window (the menu of the window, the
	// border where we could not set its color) follow our palette.
	const auto dark = BOOL(style::IsDark() ? TRUE : FALSE);
	if (FAILED(DwmSetWindowAttribute(
			_hwnd,
			kDwmwaUseImmersiveDarkMode,
			&dark,
			sizeof(dark)))) {
		DwmSetWindowAttribute(
			_hwnd,
			kDwmwaUseImmersiveDarkModeOld,
			&dark,
			sizeof(dark));
	}
}

void WinTitleFrame::recalculateFrame(bool keepClientRect) {
	if (!_installed || !_hwnd) {
		return;
	}
	const auto flags = UINT(SWP_FRAMECHANGED
		| SWP_NOZORDER
		| SWP_NOOWNERZORDER
		| SWP_NOACTIVATE);
	if (keepClientRect && !IsZoomed(_hwnd) && !IsIconic(_hwnd)) {
		// The system frame is still around the window: the window becomes
		// what its client rectangle was, so the content keeps its place
		// and its size and Qt's idea of the geometry stays true.
		auto client = RECT();
		auto window = RECT();
		if (GetClientRect(_hwnd, &client)
			&& GetWindowRect(_hwnd, &window)
			&& (client.right > client.left)
			&& (client.bottom > client.top)) {
			// Two points at once: correct for a mirrored window too.
			SetLastError(0);
			const auto mapped = MapWindowPoints(
				_hwnd,
				nullptr,
				reinterpret_cast<POINT*>(&client),
				2);
			if (mapped != 0 || GetLastError() == 0) {
				const auto left = std::min(client.left, client.right);
				const auto top = std::min(client.top, client.bottom);
				const auto width = std::abs(client.right - client.left);
				const auto height = std::abs(client.bottom - client.top);
				const auto same = (left == window.left)
					&& (top == window.top)
					&& (width == window.right - window.left)
					&& (height == window.bottom - window.top);
				if (!same
					&& SetWindowPos(
						_hwnd,
						nullptr,
						left,
						top,
						width,
						height,
						flags)) {
					return;
				}
			}
		}
	}
	SetWindowPos(
		_hwnd,
		nullptr,
		0,
		0,
		0,
		0,
		flags | SWP_NOMOVE | SWP_NOSIZE);
}

void WinTitleFrame::scheduleRefresh() {
	if (_refreshScheduled) {
		return;
	}
	_refreshScheduled = true;
	// After Qt has handled the message that brought us here.
	QTimer::singleShot(0, &_guard, [this] {
		_refreshScheduled = false;
		if (_installed && _hwnd && IsWindow(_hwnd)) {
			applyDwm();
			recalculateFrame(false);
		}
	});
}

void WinTitleFrame::windowChanged() {
	if (_installed && !_verified) {
		scheduleVerify(0);
	}
}

void WinTitleFrame::paletteChanged() {
	refreshBrush();
	applyDwm();
}

void WinTitleFrame::nativeWindowChanged() {
	if (!_window) {
		return;
	}
	// Do not create a native window from here: only follow a new one.
	if (!_window->testAttribute(Qt::WA_WState_Created)
		|| !_window->internalWinId()) {
		_installed = false;
		_hwnd = nullptr;
		return;
	}
	const auto hwnd = reinterpret_cast<HWND>(_window->internalWinId());
	if (hwnd != _hwnd) {
		install();
	}
}

void WinTitleFrame::scheduleVerify(int delay) {
	QTimer::singleShot(delay, &_guard, [this] { verifyGeometry(); });
}

// Qt must see the client rectangle the system has: it lays the widgets out in
// its own idea of it. They agree on the Qt versions this was written for; if a
// version ever does not, the content would be cut off or misplaced — the
// system frame is given back instead.
void WinTitleFrame::verifyGeometry() {
	if (_verified || !_installed || !_window || !_hwnd || !IsWindow(_hwnd)) {
		return;
	}
	const auto handle = _window->windowHandle();
	if (!handle
		|| !_window->isVisible()
		|| IsIconic(_hwnd)
		|| !IsWindowVisible(_hwnd)
		|| !hasFrameStyles()) {
		// Checked when the window is seen the next time.
		return;
	}
	auto client = RECT();
	if (!GetClientRect(_hwnd, &client)) {
		return;
	}
	const auto ratio = handle->devicePixelRatio();
	const auto width = int(std::lround(handle->width() * ratio));
	const auto height = int(std::lround(handle->height() * ratio));
	const auto good = (std::abs(width - int(client.right - client.left))
			<= kGeometryTolerance)
		&& (std::abs(height - int(client.bottom - client.top))
			<= kGeometryTolerance);
	if (good) {
		_verified = true;
		return;
	}
	// Geometry events may still be on their way: look once more later.
	if (++_verifyFailures < 3) {
		scheduleVerify(kVerifyRetryDelay);
		return;
	}
	qWarning("The custom window frame does not fit this Qt version; "
		"using the system title bar.");
	uninstall();
}

TitleHit::Zone WinTitleFrame::zoneAt(POINT screen) const {
	auto window = RECT();
	if (!_hwnd || !GetWindowRect(_hwnd, &window)) {
		return TitleHit::Zone::Client;
	}
	const auto dpi = DpiFor(_hwnd);
	const auto sizeFrameX = MetricFor(SM_CXSIZEFRAME, dpi);
	const auto sizeFrameY = MetricFor(SM_CYSIZEFRAME, dpi);
	const auto padded = MetricFor(SM_CXPADDEDBORDER, dpi);
	const auto maximized = (IsZoomed(_hwnd) != FALSE);
	const auto style = GetWindowLongPtrW(_hwnd, GWL_STYLE);
	const auto resizable = ((style & WS_THICKFRAME) != 0)
		&& _window
		&& (_window->minimumSize() != _window->maximumSize());

	auto request = TitleHit::HitTestRequest();
	request.window = FromRECT(window);
	request.client = TitleHit::ClientRectForWindow(
		request.window,
		maximized,
		TitleHit::MaximizedOverhang(sizeFrameX, padded),
		TitleHit::MaximizedOverhang(sizeFrameY, padded),
		maximized ? _autoHide : TitleHit::AutoHideEdges());
	request.cursor = { screen.x, screen.y };
	// What Qt scales the widgets by; the window DPI when it does not say.
	const auto ratio = _window ? _window->devicePixelRatioF() : 0.;
	request.scale = (ratio > 0.) ? ratio : TitleHit::ScaleForDpi(int(dpi));
	if (resizable) {
		request.borderX = TitleHit::ResizeBorderThickness(sizeFrameX, padded);
		request.borderY = TitleHit::ResizeBorderThickness(sizeFrameY, padded);
	}
	request.maximized = maximized;
	request.barVisible = _title && _window && _title->isVisibleTo(_window);
	if (request.barVisible) {
		request.bar = _title->layout();
	}
	return TitleHit::HitTest(request);
}

bool WinTitleFrame::handleCalcSize(MSG *msg, qintptr *result) {
	if (!msg->lParam) {
		return false;
	}
	// With wParam TRUE lParam is a NCCALCSIZE_PARAMS, otherwise a RECT; both
	// start with the window rectangle that is to become the client one.
	const auto rect = (msg->wParam != FALSE)
		? &reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam)->rgrc[0]
		: reinterpret_cast<RECT*>(msg->lParam);
	if (IsZoomed(_hwnd)) {
		const auto dpi = DpiFor(_hwnd);
		const auto padded = MetricFor(SM_CXPADDEDBORDER, dpi);
		_autoHide = AutoHideEdgesFor(_hwnd);
		*rect = ToRECT(TitleHit::ClientRectForWindow(
			FromRECT(*rect),
			true,
			TitleHit::MaximizedOverhang(
				MetricFor(SM_CXSIZEFRAME, dpi),
				padded),
			TitleHit::MaximizedOverhang(
				MetricFor(SM_CYSIZEFRAME, dpi),
				padded),
			_autoHide));
	} else {
		_autoHide = TitleHit::AutoHideEdges();
	}
	// Zero with the rectangle (left as it was, or cut back): this is the
	// client area, and what was valid in it stays where it is.
	*result = 0;
	return true;
}

bool WinTitleFrame::handleHitTest(MSG *msg, qintptr *result) {
	// Screen coordinates; negative on monitors left of / above the primary.
	const auto point = POINT{
		GET_X_LPARAM(msg->lParam),
		GET_Y_LPARAM(msg->lParam),
	};
	*result = HitCode(zoneAt(point));
	return true;
}

void WinTitleFrame::trackNonClientLeave() {
	auto track = TRACKMOUSEEVENT();
	track.cbSize = sizeof(track);
	track.dwFlags = TME_LEAVE | TME_NONCLIENT;
	track.hwndTrack = _hwnd;
	track.dwHoverTime = HOVER_DEFAULT;
	TrackMouseEvent(&track);
}

void WinTitleFrame::clearNativeMouse() {
	if (_title) {
		_title->setNativePressed(Button::None);
		_title->setNativeOver(Button::None);
	}
}

// The mouse over the maximize button (HTMAXBUTTON) never reaches the widget:
// hover, press and click are passed to it from the non-client messages. They
// are not given to the default handler, which would paint the button of the
// classic frame under the pointer.
bool WinTitleFrame::handleNcMouse(MSG *msg, qintptr *result) {
	const auto overMaximize = (msg->wParam == HTMAXBUTTON);
	switch (msg->message) {
	case WM_NCMOUSEMOVE:
		if (!overMaximize) {
			clearNativeMouse();
			return false;
		}
		if (_title) {
			// No tooltip where the snap layouts appear at the same place.
			_title->setNativeOver(Button::Maximize, !_snapLayouts);
		}
		trackNonClientLeave();
		*result = 0;
		return true;
	case WM_NCLBUTTONDOWN:
	case WM_NCLBUTTONDBLCLK:
		if (!overMaximize) {
			clearNativeMouse();
			return false;
		}
		if (_title) {
			_title->setNativeOver(Button::Maximize, false);
			_title->setNativePressed(Button::Maximize);
		}
		trackNonClientLeave();
		*result = 0;
		return true;
	case WM_NCLBUTTONUP:
		if (!overMaximize) {
			clearNativeMouse();
			return false;
		}
		if (_title && _title->nativePressed() == Button::Maximize) {
			_title->setNativePressed(Button::None);
			// Out of the window procedure: the click changes the window
			// state.
			QTimer::singleShot(0, _title.data(), [title = _title] {
				if (title) {
					title->nativeClick(Button::Maximize);
				}
			});
		}
		*result = 0;
		return true;
	case WM_NCRBUTTONUP:
		if (msg->wParam != HTCAPTION) {
			return false;
		}
		showSystemMenu(POINT{
			GET_X_LPARAM(msg->lParam),
			GET_Y_LPARAM(msg->lParam),
		});
		*result = 0;
		return true;
	}
	return false;
}

void WinTitleFrame::showSystemMenu(POINT screen) {
	const auto hwnd = _hwnd;
	const auto menu = GetSystemMenu(hwnd, FALSE);
	if (!menu) {
		return;
	}
	const auto maximized = (IsZoomed(hwnd) != FALSE);
	const auto set = [&](UINT item, bool enabled) {
		EnableMenuItem(
			menu,
			item,
			MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
	};
	set(SC_RESTORE, maximized);
	set(SC_MOVE, !maximized);
	set(SC_SIZE, !maximized);
	set(SC_MINIMIZE, true);
	set(SC_MAXIMIZE, !maximized);
	set(SC_CLOSE, true);
	SetMenuDefaultItem(menu, SC_CLOSE, FALSE);

	const auto align = GetSystemMetrics(SM_MENUDROPALIGNMENT)
		? TPM_RIGHTALIGN
		: TPM_LEFTALIGN;
	// Runs the menu; the frame may be gone when it returns.
	const auto guard = QPointer<QObject>(&_guard);
	const auto command = TrackPopupMenu(
		menu,
		TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_TOPALIGN | align,
		screen.x,
		screen.y,
		0,
		hwnd,
		nullptr);
	if (command && guard && IsWindow(hwnd)) {
		PostMessageW(hwnd, WM_SYSCOMMAND, WPARAM(command), 0);
	}
}

bool WinTitleFrame::nativeEvent(void *message, qintptr *result) {
	const auto msg = static_cast<MSG*>(message);
	if (!_installed || !msg || !result || !_hwnd || msg->hwnd != _hwnd) {
		return false;
	}
	switch (msg->message) {
	case WM_NCCALCSIZE:
		return hasFrameStyles() && handleCalcSize(msg, result);
	case WM_NCHITTEST:
		return hasFrameStyles() && handleHitTest(msg, result);
	case WM_NCACTIVATE:
		if (!hasFrameStyles()) {
			return false;
		}
		// -1: the default handler changes the active state and repaints
		// nothing of the non-client area. Its answer is passed on (TRUE
		// lets the window be deactivated).
		*result = DefWindowProcW(
			msg->hwnd,
			WM_NCACTIVATE,
			msg->wParam,
			LPARAM(-1));
		return true;
	case WM_NCPAINT: {
		if (!hasFrameStyles()) {
			return false;
		}
		// With composition the default handler paints nothing of ours and
		// the shadow is its business; without it the classic frame would
		// be painted over the content.
		auto composition = BOOL(FALSE);
		if (SUCCEEDED(DwmIsCompositionEnabled(&composition)) && composition) {
			return false;
		}
		*result = 0;
		return true;
	}
	case kWmNcUahDrawCaption:
	case kWmNcUahDrawFrame:
		if (!hasFrameStyles()) {
			return false;
		}
		*result = 0;
		return true;
	case WM_ERASEBKGND: {
		const auto dc = reinterpret_cast<HDC>(msg->wParam);
		auto client = RECT();
		if (dc && _brush && GetClientRect(msg->hwnd, &client)) {
			FillRect(dc, &client, _brush);
		}
		// Erased.
		*result = 1;
		return true;
	}
	case WM_NCMOUSEMOVE:
	case WM_NCLBUTTONDOWN:
	case WM_NCLBUTTONDBLCLK:
	case WM_NCLBUTTONUP:
	case WM_NCRBUTTONUP:
		return hasFrameStyles() && handleNcMouse(msg, result);
	case WM_NCMOUSELEAVE:
	case WM_MOUSEMOVE:
	case WM_KILLFOCUS:
	case WM_CAPTURECHANGED:
		// The pointer is not over the maximize button any more.
		clearNativeMouse();
		return false;
	case WM_DPICHANGED:
	case WM_THEMECHANGED:
	case WM_DWMCOMPOSITIONCHANGED:
	case WM_SETTINGCHANGE:
		// The frame metrics, the taskbar or the system colors changed (Qt
		// also resets the dark mode flag of the window on a system theme
		// change): once Qt is done, put ours back and have the client
		// rectangle computed again.
		scheduleRefresh();
		return false;
	}
	return false;
}

} // namespace

std::unique_ptr<TitleFrame> CreateTitleFrame(
		QWidget *window,
		TitleBar *title,
		std::function<void()> lost) {
	if (!window || !title || !CustomTitleBarSupported()) {
		return nullptr;
	}
	auto result = std::make_unique<WinTitleFrame>(
		window,
		title,
		std::move(lost));
	if (!result->installed()) {
		return nullptr;
	}
	return result;
}

} // namespace Window
