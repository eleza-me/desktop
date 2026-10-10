// The arithmetic of the custom window frame (window/window_title_hit_test.h):
// unit conversions, the layout of the title bar, the client rectangle of a
// frameless window and what is under the cursor.
#include "window/window_title_hit_test.h"

#include <QtTest/QtTest>

#include <array>
#include <cmath>

using namespace Window::TitleHit;

namespace {

// The values of window.style, in logical pixels.
constexpr auto kBarHeight = 30;
constexpr auto kButtonWidth = 46;
constexpr auto kWindowWidth = 1000;
constexpr auto kWindowHeight = 700;

constexpr auto kScales = std::array{ 1., 1.25, 1.5, 2. };
// The primary monitor, one to the left and above it, one to the right.
constexpr auto kOrigins = std::array{
	Point{ 0, 0 },
	Point{ -2560, -360 },
	Point{ 1920, 120 },
};

// What the system reports for the size frame and the padded border.
[[nodiscard]] int FrameMetric(double scale) {
	return int(std::lround(4 * scale));
}

struct Sample {
	HitTestRequest request;
	int border = 0;
	int overhang = 0;
	// The monitor a maximized window fills.
	Rect monitor;
};

[[nodiscard]] Sample MakeNormal(Point origin, double scale, bool rtl) {
	auto result = Sample();
	result.border = ResizeBorderThickness(
		FrameMetric(scale),
		FrameMetric(scale));
	auto &request = result.request;
	request.window = {
		origin.x,
		origin.y,
		origin.x + ToPhysical(kWindowWidth, scale),
		origin.y + ToPhysical(kWindowHeight, scale),
	};
	request.client = ClientRectForWindow(request.window, false, 0, 0);
	request.scale = scale;
	request.borderX = request.borderY = result.border;
	request.bar = ComputeBarLayout(
		kWindowWidth,
		kBarHeight,
		kButtonWidth,
		rtl);
	return result;
}

[[nodiscard]] Sample MakeMaximized(
		Point origin,
		double scale,
		bool rtl,
		const AutoHideEdges &autoHide = AutoHideEdges()) {
	auto result = MakeNormal(origin, scale, rtl);
	result.overhang = MaximizedOverhang(
		FrameMetric(scale),
		FrameMetric(scale));
	auto &request = result.request;
	result.monitor = request.window;
	request.window = {
		result.monitor.left - result.overhang,
		result.monitor.top - result.overhang,
		result.monitor.right + result.overhang,
		result.monitor.bottom + result.overhang,
	};
	request.client = ClientRectForWindow(
		request.window,
		true,
		result.overhang,
		result.overhang,
		autoHide);
	request.maximized = true;
	return result;
}

[[nodiscard]] Zone At(const HitTestRequest &request, int x, int y) {
	auto copy = request;
	copy.cursor = { x, y };
	return HitTest(copy);
}

// A point of the bar given in logical pixels of the client area.
[[nodiscard]] Zone AtLogical(
		const HitTestRequest &request,
		int logicalX,
		int logicalY) {
	return At(
		request,
		request.client.left + ToPhysical(logicalX, request.scale),
		request.client.top + ToPhysical(logicalY, request.scale));
}

} // namespace

class TestTitleHitTest : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void units() {
		QCOMPARE(ScaleForDpi(96), 1.);
		QCOMPARE(ScaleForDpi(120), 1.25);
		QCOMPARE(ScaleForDpi(144), 1.5);
		QCOMPARE(ScaleForDpi(192), 2.);
		// Nonsense from the system must not turn the window inside out.
		QCOMPARE(ScaleForDpi(0), 1.);
		QCOMPARE(ScaleForDpi(-96), 1.);
		QCOMPARE(ScaleForDpi(96 * 100), 1.);

		QCOMPARE(ToPhysical(30, 1.), 30);
		QCOMPARE(ToPhysical(30, 1.25), 38); // 37.5 goes up
		QCOMPARE(ToPhysical(46, 1.25), 58); // 57.5 goes up
		QCOMPARE(ToPhysical(46, 1.5), 69);
		QCOMPARE(ToPhysical(30, 2.), 60);
		QCOMPARE(ToPhysical(-30, 1.5), -45);
		QCOMPARE(ToPhysical(30, 0.), 30);

		QCOMPARE(ToLogical(38, 1.25), 30);
		QCOMPARE(ToLogical(37, 1.25), 29);
		QCOMPARE(ToLogical(59, 2.), 29);
		QCOMPARE(ToLogical(60, 2.), 30);
		// Left of / above the origin stays there.
		QCOMPARE(ToLogical(-1, 2.), -1);
		QCOMPARE(ToLogical(-1, 1.), -1);
		QCOMPARE(ToLogical(-3, 1.5), -2);

		const auto rect = ToPhysical(Rect{ 908, 0, 954, 30 }, 1.5);
		QCOMPARE(rect, (Rect{ 1362, 0, 1431, 45 }));
		QCOMPARE(rect.width(), 69);
		QCOMPARE(rect.height(), 45);
	}

	void frameMetrics() {
		// Size frame 4 + padded border 4 at 100 %.
		QCOMPARE(MaximizedOverhang(4, 4), 8);
		QCOMPARE(MaximizedOverhang(8, 8), 16);
		QCOMPARE(MaximizedOverhang(-1, 4), 4);
		QCOMPARE(ResizeBorderThickness(4, 4), 6);
		QCOMPARE(ResizeBorderThickness(5, 5), 7);
		QCOMPARE(ResizeBorderThickness(6, 6), 9);
		QCOMPARE(ResizeBorderThickness(8, 8), 12);
		// Never too thin to catch.
		QCOMPARE(ResizeBorderThickness(0, 0), 4);
		QCOMPARE(ResizeBorderThickness(1, 0), 4);
	}

	void barLayout() {
		for (const auto rtl : { false, true }) {
			const auto layout = ComputeBarLayout(
				kWindowWidth,
				kBarHeight,
				kButtonWidth,
				rtl);
			QCOMPARE(layout.bar, (Rect{ 0, 0, 1000, 30 }));
			// At the physical right in both interface directions.
			QCOMPARE(layout.close, (Rect{ 954, 0, 1000, 30 }));
			QCOMPARE(layout.maximize, (Rect{ 908, 0, 954, 30 }));
			QCOMPARE(layout.minimize, (Rect{ 862, 0, 908, 30 }));
			QCOMPARE(layout.caption, (Rect{ 0, 0, 862, 30 }));
			// Only the title follows the direction.
			QCOMPARE(layout.titleFromRight, rtl);
		}
	}

	void barLayoutTooNarrow() {
		const auto layout = ComputeBarLayout(100, 30, 46, false);
		QCOMPARE(layout.close, (Rect{ 54, 0, 100, 30 }));
		QCOMPARE(layout.maximize, (Rect{ 8, 0, 54, 30 }));
		QCOMPARE(layout.minimize, (Rect{ 0, 0, 8, 30 }));
		QVERIFY(layout.caption.empty());

		const auto none = ComputeBarLayout(-5, -1, 46, true);
		QVERIFY(none.bar.empty());
		QVERIFY(none.close.empty());
		QVERIFY(none.caption.empty());
	}

	void clientRect() {
		const auto window = Rect{ -8, -8, 1928, 1048 };
		// A normal window is all client.
		QCOMPARE(ClientRectForWindow(window, false, 8, 8), window);
		QCOMPARE(
			ClientRectForWindow(Rect{ -500, -300, 500, 400 }, false, 8, 8),
			(Rect{ -500, -300, 500, 400 }));
		// A maximized one is cut back to the monitor.
		QCOMPARE(
			ClientRectForWindow(window, true, 8, 8),
			(Rect{ 0, 0, 1920, 1040 }));
		// On a monitor to the left of the primary one, at 150 %.
		QCOMPARE(
			ClientRectForWindow(Rect{ -2572, -372, 12, 1092 }, true, 12, 12),
			(Rect{ -2560, -360, 0, 1080 }));
		// Different thickness per axis.
		QCOMPARE(
			ClientRectForWindow(window, true, 8, 6),
			(Rect{ 0, -2, 1920, 1042 }));
		// A negative thickness is no thickness.
		QCOMPARE(ClientRectForWindow(window, true, -3, -3), window);
	}

	void clientRectAutoHideTaskbar() {
		const auto window = Rect{ -8, -8, 1928, 1088 };
		auto edges = AutoHideEdges();
		edges.bottom = true;
		QCOMPARE(
			ClientRectForWindow(window, true, 8, 8, edges),
			(Rect{ 0, 0, 1920, 1079 }));
		edges = AutoHideEdges();
		edges.top = true;
		QCOMPARE(
			ClientRectForWindow(window, true, 8, 8, edges),
			(Rect{ 0, 1, 1920, 1080 }));
		edges = AutoHideEdges();
		edges.left = true;
		QCOMPARE(
			ClientRectForWindow(window, true, 8, 8, edges),
			(Rect{ 1, 0, 1920, 1080 }));
		edges = AutoHideEdges();
		edges.right = true;
		QCOMPARE(
			ClientRectForWindow(window, true, 8, 8, edges),
			(Rect{ 0, 0, 1919, 1080 }));
		// Only a maximized window makes room.
		QCOMPARE(ClientRectForWindow(window, false, 8, 8, edges), window);
	}

	void clientRectDegenerate() {
		// Smaller than its frame: left alone.
		const auto tiny = Rect{ 10, 10, 20, 20 };
		QCOMPARE(ClientRectForWindow(tiny, true, 8, 8), tiny);
	}

	void outside() {
		const auto sample = MakeNormal({ 100, 100 }, 1., false);
		const auto &window = sample.request.window;
		QCOMPARE(At(sample.request, window.left - 1, window.top), Zone::Outside);
		QCOMPARE(At(sample.request, window.left, window.top - 1), Zone::Outside);
		// Right and bottom are exclusive.
		QCOMPARE(At(sample.request, window.right, window.top), Zone::Outside);
		QCOMPARE(At(sample.request, window.left, window.bottom), Zone::Outside);
		QCOMPARE(
			At(sample.request, window.right - 1, window.bottom - 1),
			Zone::BottomRight);
	}

	void borders() {
		for (const auto scale : kScales) {
			for (const auto origin : kOrigins) {
				for (const auto rtl : { false, true }) {
					const auto sample = MakeNormal(origin, scale, rtl);
					const auto &request = sample.request;
					const auto &window = request.window;
					const auto border = sample.border;
					const auto corner = 2 * border;
					const auto middleX = (window.left + window.right) / 2;
					const auto middleY = (window.top + window.bottom) / 2;

					// ---- edges: the whole thickness, and not a pixel more.
					QCOMPARE(At(request, window.left, middleY), Zone::Left);
					QCOMPARE(
						At(request, window.left + border - 1, middleY),
						Zone::Left);
					QCOMPARE(
						At(request, window.left + border, middleY),
						Zone::Client);
					QCOMPARE(At(request, window.right - 1, middleY), Zone::Right);
					QCOMPARE(
						At(request, window.right - border, middleY),
						Zone::Right);
					QCOMPARE(
						At(request, window.right - border - 1, middleY),
						Zone::Client);
					QCOMPARE(At(request, middleX, window.top), Zone::Top);
					QCOMPARE(
						At(request, middleX, window.top + border - 1),
						Zone::Top);
					// Below the top border the bar begins.
					QCOMPARE(
						At(request, middleX, window.top + border),
						Zone::Caption);
					QCOMPARE(
						At(request, middleX, window.bottom - 1),
						Zone::Bottom);
					QCOMPARE(
						At(request, middleX, window.bottom - border),
						Zone::Bottom);
					QCOMPARE(
						At(request, middleX, window.bottom - border - 1),
						Zone::Client);

					// ---- corners: twice the thickness along both edges.
					QCOMPARE(At(request, window.left, window.top), Zone::TopLeft);
					QCOMPARE(
						At(request, window.left + corner - 1, window.top),
						Zone::TopLeft);
					QCOMPARE(
						At(request, window.left + corner, window.top),
						Zone::Top);
					QCOMPARE(
						At(request, window.left, window.top + corner - 1),
						Zone::TopLeft);
					QCOMPARE(
						At(request, window.left, window.top + corner),
						Zone::Left);

					QCOMPARE(
						At(request, window.right - 1, window.top),
						Zone::TopRight);
					QCOMPARE(
						At(request, window.right - corner, window.top),
						Zone::TopRight);
					QCOMPARE(
						At(request, window.right - corner - 1, window.top),
						Zone::Top);
					QCOMPARE(
						At(request, window.right - 1, window.top + corner - 1),
						Zone::TopRight);
					QCOMPARE(
						At(request, window.right - 1, window.top + corner),
						Zone::Right);

					QCOMPARE(
						At(request, window.left, window.bottom - 1),
						Zone::BottomLeft);
					QCOMPARE(
						At(request, window.left + corner - 1, window.bottom - 1),
						Zone::BottomLeft);
					QCOMPARE(
						At(request, window.left + corner, window.bottom - 1),
						Zone::Bottom);
					QCOMPARE(
						At(request, window.left, window.bottom - corner),
						Zone::BottomLeft);
					QCOMPARE(
						At(request, window.left, window.bottom - corner - 1),
						Zone::Left);

					QCOMPARE(
						At(request, window.right - 1, window.bottom - 1),
						Zone::BottomRight);
					QCOMPARE(
						At(request, window.right - corner, window.bottom - 1),
						Zone::BottomRight);
					QCOMPARE(
						At(request, window.right - corner - 1, window.bottom - 1),
						Zone::Bottom);
					QCOMPARE(
						At(request, window.right - 1, window.bottom - corner),
						Zone::BottomRight);
					QCOMPARE(
						At(request, window.right - 1, window.bottom - corner - 1),
						Zone::Right);

					// Inside a corner square but off both borders: content.
					QCOMPARE(
						At(
							request,
							window.left + border,
							window.bottom - border - 1),
						Zone::Client);
				}
			}
		}
	}

	void topBorderAboveButtons() {
		for (const auto scale : kScales) {
			for (const auto origin : kOrigins) {
				const auto sample = MakeNormal(origin, scale, false);
				const auto &request = sample.request;
				const auto &bar = request.bar;
				for (const auto &button : {
						bar.minimize,
						bar.maximize,
						bar.close }) {
					const auto x = request.client.left
						+ ToPhysical((button.left + button.right) / 2, scale);
					// The strip at the very top still resizes the window…
					QCOMPARE(At(request, x, request.window.top), Zone::Top);
					QCOMPARE(
						At(request, x, request.window.top + sample.border - 1),
						Zone::Top);
					// …and right below it the button begins.
					QVERIFY(At(request, x, request.window.top + sample.border)
						!= Zone::Top);
				}
			}
		}
	}

	void captionAndButtons() {
		for (const auto scale : kScales) {
			for (const auto origin : kOrigins) {
				for (const auto rtl : { false, true }) {
					const auto sample = MakeNormal(origin, scale, rtl);
					const auto &request = sample.request;
					const auto y = kBarHeight / 2;
					// The same places in both interface directions.
					QCOMPARE(AtLogical(request, 977, y), Zone::Close);
					QCOMPARE(AtLogical(request, 931, y), Zone::Maximize);
					QCOMPARE(AtLogical(request, 885, y), Zone::Minimize);
					QCOMPARE(AtLogical(request, 500, y), Zone::Caption);
					QCOMPARE(AtLogical(request, 40, y), Zone::Caption);

					// ---- the seams between them.
					QCOMPARE(AtLogical(request, 954, y), Zone::Close);
					QCOMPARE(AtLogical(request, 953, y), Zone::Maximize);
					QCOMPARE(AtLogical(request, 908, y), Zone::Maximize);
					QCOMPARE(AtLogical(request, 907, y), Zone::Minimize);
					QCOMPARE(AtLogical(request, 862, y), Zone::Minimize);
					QCOMPARE(AtLogical(request, 861, y), Zone::Caption);

					// ---- the lower edge of the bar.
					QCOMPARE(
						AtLogical(request, 500, kBarHeight - 1),
						Zone::Caption);
					QCOMPARE(
						AtLogical(request, 500, kBarHeight),
						Zone::Client);
					QCOMPARE(
						AtLogical(request, 977, kBarHeight - 1),
						Zone::Close);
					QCOMPARE(
						AtLogical(request, 977, kBarHeight),
						Zone::Client);
					QCOMPARE(AtLogical(request, 500, 400), Zone::Client);
				}
			}
		}
	}

	void physicalSeams() {
		// At 125 % the close button starts at 954 * 1.25 = 1192.5: the
		// physical pixel 1192 is still the maximize button, 1193 is close.
		const auto sample = MakeNormal({ 0, 0 }, 1.25, false);
		QCOMPARE(At(sample.request, 1192, 20), Zone::Maximize);
		QCOMPARE(At(sample.request, 1193, 20), Zone::Close);
		// The bar is 37.5 physical pixels high: row 37 is the last of it.
		QCOMPARE(At(sample.request, 600, 37), Zone::Caption);
		QCOMPARE(At(sample.request, 600, 38), Zone::Client);
	}

	void maximized() {
		for (const auto scale : kScales) {
			for (const auto origin : kOrigins) {
				for (const auto rtl : { false, true }) {
					const auto sample = MakeMaximized(origin, scale, rtl);
					const auto &request = sample.request;
					const auto &monitor = sample.monitor;
					// The client area is exactly the monitor.
					QCOMPARE(request.client, monitor);
					QCOMPARE(
						request.window.width(),
						monitor.width() + 2 * sample.overhang);

					// No resize borders: the screen corners are the bar.
					QCOMPARE(
						At(request, monitor.left, monitor.top),
						Zone::Caption);
					QCOMPARE(
						At(request, monitor.right - 1, monitor.top),
						Zone::Close);
					QCOMPARE(
						At(request, monitor.left, monitor.bottom - 1),
						Zone::Client);
					QCOMPARE(
						At(request, monitor.right - 1, monitor.bottom - 1),
						Zone::Client);
					const auto middleY = (monitor.top + monitor.bottom) / 2;
					QCOMPARE(At(request, monitor.left, middleY), Zone::Client);
					QCOMPARE(
						At(request, monitor.right - 1, middleY),
						Zone::Client);

					const auto y = kBarHeight / 2;
					QCOMPARE(AtLogical(request, 977, y), Zone::Close);
					QCOMPARE(AtLogical(request, 931, y), Zone::Maximize);
					QCOMPARE(AtLogical(request, 885, y), Zone::Minimize);
					QCOMPARE(AtLogical(request, 500, y), Zone::Caption);
					QCOMPARE(AtLogical(request, 500, kBarHeight), Zone::Client);

					// The part that hangs over the monitor belongs to what
					// is next to it, never to a border.
					QCOMPARE(
						At(request, request.window.right - 1, request.window.top),
						Zone::Close);
					QCOMPARE(
						At(request, request.window.left, request.window.top),
						Zone::Caption);
					QCOMPARE(
						At(
							request,
							request.window.left,
							request.window.bottom - 1),
						Zone::Client);
				}
			}
		}
	}

	void maximizedWithAutoHideTaskbar() {
		for (const auto scale : kScales) {
			for (const auto origin : kOrigins) {
				auto edges = AutoHideEdges();
				edges.top = true;
				edges.right = true;
				const auto sample = MakeMaximized(origin, scale, false, edges);
				const auto &request = sample.request;
				const auto &monitor = sample.monitor;
				QCOMPARE(request.client.top, monitor.top + 1);
				QCOMPARE(request.client.right, monitor.right - 1);
				// The pixel row and column kept for the taskbar still hit
				// the button in the corner of the screen.
				QCOMPARE(
					At(request, monitor.right - 1, monitor.top),
					Zone::Close);
				QCOMPARE(
					At(request, monitor.right - 2, monitor.top + 1),
					Zone::Close);
				QCOMPARE(At(request, monitor.left, monitor.top), Zone::Caption);
			}
		}
	}

	void notResizable() {
		auto sample = MakeNormal({ -400, -200 }, 1.5, true);
		auto &request = sample.request;
		request.borderX = request.borderY = 0;
		const auto &window = request.window;
		QCOMPARE(At(request, window.left, window.top), Zone::Caption);
		QCOMPARE(At(request, window.right - 1, window.top), Zone::Close);
		QCOMPARE(At(request, window.left, window.bottom - 1), Zone::Client);
	}

	void barHidden() {
		// Fullscreen: no bar, the top of the window is content.
		for (const auto scale : kScales) {
			auto sample = MakeNormal({ 0, 0 }, scale, false);
			auto &request = sample.request;
			request.barVisible = false;
			request.borderX = request.borderY = 0;
			QCOMPARE(AtLogical(request, 977, 10), Zone::Client);
			QCOMPARE(AtLogical(request, 500, 10), Zone::Client);
			QCOMPARE(At(request, 0, 0), Zone::Client);
		}
	}

	void negativeOrigins() {
		// A window that straddles the origin of the virtual screen.
		const auto scale = 1.5;
		auto sample = MakeNormal({ -900, -30 }, scale, true);
		const auto &request = sample.request;
		QCOMPARE(request.window, (Rect{ -900, -30, 600, 1020 }));
		// Screen x = -1 and x = 0 are neighbours inside the bar.
		QCOMPARE(At(request, -1, -5), Zone::Caption);
		QCOMPARE(At(request, 0, -5), Zone::Caption);
		QCOMPARE(At(request, -1, 0), Zone::Caption);
		// The bar ends at -30 + 45 = 15.
		QCOMPARE(At(request, -1, 14), Zone::Caption);
		QCOMPARE(At(request, -1, 15), Zone::Client);
		// The close button: 954 * 1.5 = 1431 -> x = 531.
		QCOMPARE(At(request, 530, 0), Zone::Maximize);
		QCOMPARE(At(request, 531, 0), Zone::Close);
		QCOMPARE(At(request, -900, -30), Zone::TopLeft);
		QCOMPARE(At(request, -901, -30), Zone::Outside);
	}

	void tinyWindow() {
		// Borders never overlap past the middle of a very small window.
		auto request = HitTestRequest();
		request.window = { 0, 0, 10, 10 };
		request.client = request.window;
		request.borderX = request.borderY = 8;
		request.barVisible = false;
		QCOMPARE(At(request, 0, 0), Zone::TopLeft);
		QCOMPARE(At(request, 9, 9), Zone::BottomRight);
		QCOMPARE(At(request, 9, 0), Zone::TopRight);
		QCOMPARE(At(request, 0, 9), Zone::BottomLeft);
	}

	void resizeZones() {
		for (const auto zone : {
				Zone::Left,
				Zone::Top,
				Zone::Right,
				Zone::Bottom,
				Zone::TopLeft,
				Zone::TopRight,
				Zone::BottomLeft,
				Zone::BottomRight }) {
			QVERIFY(IsResizeZone(zone));
		}
		for (const auto zone : {
				Zone::Outside,
				Zone::Client,
				Zone::Caption,
				Zone::Minimize,
				Zone::Maximize,
				Zone::Close }) {
			QVERIFY(!IsResizeZone(zone));
		}
	}

};

QTEST_GUILESS_MAIN(TestTitleHitTest)
#include "test_title_hit_test.moc"
