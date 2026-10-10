// The arithmetic of the custom window frame, without any toolkit or platform
// type, so it is unit-tested on every platform (tests/app/test_title_hit_test.cpp):
//
//  * unit conversions between logical pixels (what widgets use) and physical
//    pixels (what the window system uses);
//  * the layout of the title bar: where the three window buttons and the
//    draggable part are;
//  * the client rectangle of a window whose non-client area was removed
//    (maximized windows hang over the monitor by the frame thickness);
//  * what is under the cursor: a resize border, the draggable part of the bar,
//    one of the buttons or the content.
//
// Rectangles are half-open: [left, right) x [top, bottom), as the window
// system reports them.
#pragma once

namespace Window::TitleHit {

struct Point {
	int x = 0;
	int y = 0;
};

struct Rect {
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;

	[[nodiscard]] constexpr int width() const {
		return right - left;
	}
	[[nodiscard]] constexpr int height() const {
		return bottom - top;
	}
	[[nodiscard]] constexpr bool empty() const {
		return (right <= left) || (bottom <= top);
	}
	[[nodiscard]] constexpr bool contains(int x, int y) const {
		return (x >= left) && (x < right) && (y >= top) && (y < bottom);
	}
	[[nodiscard]] constexpr bool contains(Point point) const {
		return contains(point.x, point.y);
	}

	friend constexpr bool operator==(const Rect &a, const Rect &b) {
		return (a.left == b.left)
			&& (a.top == b.top)
			&& (a.right == b.right)
			&& (a.bottom == b.bottom);
	}
	friend constexpr bool operator!=(const Rect &a, const Rect &b) {
		return !(a == b);
	}
};

// ---- units

// Physical pixels per logical pixel for a window DPI (96 = 1.0). A DPI that
// makes no sense gives 1.
[[nodiscard]] double ScaleForDpi(int dpi);
// Rounded to the nearest physical pixel (halves go up).
[[nodiscard]] int ToPhysical(int logical, double scale);
[[nodiscard]] Rect ToPhysical(const Rect &logical, double scale);
// The logical pixel a physical one falls into (towards minus infinity, so
// positions left of / above the origin stay there).
[[nodiscard]] int ToLogical(int physical, double scale);

// ---- the bar

// In logical pixels, relative to the top left corner of the bar.
struct BarLayout {
	Rect bar;
	// The window buttons: always at the physical right, in this order from the
	// left, whatever the interface direction is.
	Rect minimize;
	Rect maximize;
	Rect close;
	// What is left of the bar: the window is dragged by it and the title is
	// painted in it.
	Rect caption;
	// The title starts at the right end of `caption` (right-to-left
	// interface) or at its left end.
	bool titleFromRight = false;
};

[[nodiscard]] BarLayout ComputeBarLayout(
	int width,
	int height,
	int buttonWidth,
	bool rightToLeft);

// ---- the frame

// Thickness of the resize border inside the window, from the system metrics
// for the size frame and the padded border at the window DPI (physical
// pixels). The system frame of a normal window lies outside of what is seen;
// ours takes its strip from the content, so it is a little thinner.
[[nodiscard]] int ResizeBorderThickness(int sizeFrame, int paddedBorder);
// How far a maximized window hangs over each edge of its monitor.
[[nodiscard]] int MaximizedOverhang(int sizeFrame, int paddedBorder);

// Edges of the monitor that hold an auto-hide taskbar.
struct AutoHideEdges {
	bool left = false;
	bool top = false;
	bool right = false;
	bool bottom = false;
};

// The client rectangle for a window rectangle (same coordinates, physical
// pixels). A normal window is all client. A maximized one is cut back to the
// monitor, and where an auto-hide taskbar waits one more pixel is left out,
// so the pointer still reaches the taskbar.
[[nodiscard]] Rect ClientRectForWindow(
	const Rect &window,
	bool maximized,
	int overhangX,
	int overhangY,
	const AutoHideEdges &autoHide = AutoHideEdges());

enum class Zone {
	Outside,
	Client,
	Caption,
	Minimize,
	Maximize,
	Close,
	Left,
	Top,
	Right,
	Bottom,
	TopLeft,
	TopRight,
	BottomLeft,
	BottomRight,
};

struct HitTestRequest {
	// Screen coordinates, physical pixels. Monitors left of / above the
	// primary one have negative coordinates.
	Rect window;
	Rect client;
	Point cursor;
	// Physical pixels per logical pixel of the window.
	double scale = 1.;
	// Resize border, physical pixels. Zero: the window is not resizable.
	int borderX = 0;
	int borderY = 0;
	bool maximized = false;
	// The bar lies at the top of the client rectangle (hidden in fullscreen).
	bool barVisible = true;
	BarLayout bar;
};

[[nodiscard]] Zone HitTest(const HitTestRequest &request);

[[nodiscard]] constexpr bool IsResizeZone(Zone zone) {
	return (zone == Zone::Left)
		|| (zone == Zone::Top)
		|| (zone == Zone::Right)
		|| (zone == Zone::Bottom)
		|| (zone == Zone::TopLeft)
		|| (zone == Zone::TopRight)
		|| (zone == Zone::BottomLeft)
		|| (zone == Zone::BottomRight);
}

} // namespace Window::TitleHit
