#include "window/window_title_hit_test.h"

#include <algorithm>
#include <cmath>

namespace Window::TitleHit {
namespace {

constexpr auto kDefaultDpi = 96;
// Scales beyond this are not something a monitor reports.
constexpr auto kMaxScale = 8.;

[[nodiscard]] double SafeScale(double scale) {
	return (scale > 0. && scale <= kMaxScale) ? scale : 1.;
}

} // namespace

double ScaleForDpi(int dpi) {
	return SafeScale(double(dpi) / kDefaultDpi);
}

int ToPhysical(int logical, double scale) {
	return int(std::floor(logical * SafeScale(scale) + 0.5));
}

Rect ToPhysical(const Rect &logical, double scale) {
	return {
		ToPhysical(logical.left, scale),
		ToPhysical(logical.top, scale),
		ToPhysical(logical.right, scale),
		ToPhysical(logical.bottom, scale),
	};
}

int ToLogical(int physical, double scale) {
	return int(std::floor(physical / SafeScale(scale)));
}

BarLayout ComputeBarLayout(
		int width,
		int height,
		int buttonWidth,
		bool rightToLeft) {
	width = std::max(width, 0);
	height = std::max(height, 0);
	buttonWidth = std::max(buttonWidth, 0);

	auto result = BarLayout();
	result.bar = { 0, 0, width, height };
	// From the right edge: close, maximize, minimize. A bar narrower than
	// the three buttons keeps the close button whole.
	const auto closeLeft = std::max(width - buttonWidth, 0);
	const auto maximizeLeft = std::max(closeLeft - buttonWidth, 0);
	const auto minimizeLeft = std::max(maximizeLeft - buttonWidth, 0);
	result.close = { closeLeft, 0, width, height };
	result.maximize = { maximizeLeft, 0, closeLeft, height };
	result.minimize = { minimizeLeft, 0, maximizeLeft, height };
	result.caption = { 0, 0, minimizeLeft, height };
	result.titleFromRight = rightToLeft;
	return result;
}

int ResizeBorderThickness(int sizeFrame, int paddedBorder) {
	sizeFrame = std::max(sizeFrame, 0);
	paddedBorder = std::max(paddedBorder, 0);
	// Never thinner than something a pointer can catch.
	return std::max(sizeFrame + paddedBorder / 2, 4);
}

int MaximizedOverhang(int sizeFrame, int paddedBorder) {
	return std::max(sizeFrame, 0) + std::max(paddedBorder, 0);
}

Rect ClientRectForWindow(
		const Rect &window,
		bool maximized,
		int overhangX,
		int overhangY,
		const AutoHideEdges &autoHide) {
	if (!maximized) {
		return window;
	}
	overhangX = std::max(overhangX, 0);
	overhangY = std::max(overhangY, 0);
	auto result = Rect{
		window.left + overhangX,
		window.top + overhangY,
		window.right - overhangX,
		window.bottom - overhangY,
	};
	if (autoHide.left) {
		++result.left;
	}
	if (autoHide.top) {
		++result.top;
	}
	if (autoHide.right) {
		--result.right;
	}
	if (autoHide.bottom) {
		--result.bottom;
	}
	// A degenerate window rectangle is left alone rather than turned inside
	// out.
	return result.empty() ? window : result;
}

Zone HitTest(const HitTestRequest &request) {
	const auto &window = request.window;
	auto cursor = request.cursor;
	if (!window.contains(cursor)) {
		return Zone::Outside;
	}

	if (!request.maximized && request.borderX > 0 && request.borderY > 0) {
		const auto borderX = std::min(request.borderX, window.width() / 2);
		const auto borderY = std::min(request.borderY, window.height() / 2);
		// The corners reach further along the edges than the border is
		// thick, the way system frames do it.
		const auto cornerX = std::min(2 * borderX, window.width() / 2);
		const auto cornerY = std::min(2 * borderY, window.height() / 2);
		const auto onLeft = (cursor.x < window.left + borderX);
		const auto onRight = (cursor.x >= window.right - borderX);
		const auto onTop = (cursor.y < window.top + borderY);
		const auto onBottom = (cursor.y >= window.bottom - borderY);
		if (onLeft || onRight || onTop || onBottom) {
			const auto nearLeft = (cursor.x < window.left + cornerX);
			const auto nearRight = (cursor.x >= window.right - cornerX);
			const auto nearTop = (cursor.y < window.top + cornerY);
			const auto nearBottom = (cursor.y >= window.bottom - cornerY);
			if (nearTop && nearLeft) {
				return Zone::TopLeft;
			} else if (nearTop && nearRight) {
				return Zone::TopRight;
			} else if (nearBottom && nearLeft) {
				return Zone::BottomLeft;
			} else if (nearBottom && nearRight) {
				return Zone::BottomRight;
			} else if (onTop) {
				return Zone::Top;
			} else if (onBottom) {
				return Zone::Bottom;
			} else if (onLeft) {
				return Zone::Left;
			}
			return Zone::Right;
		}
	}

	const auto &client = request.client;
	if (client.empty()) {
		return Zone::Client;
	}
	if (request.maximized) {
		// The pixel row kept free for an auto-hide taskbar and the edge of
		// the screen belong to what is next to them: throwing the pointer
		// into the top right corner must land on the close button.
		cursor.x = std::clamp(cursor.x, client.left, client.right - 1);
		cursor.y = std::clamp(cursor.y, client.top, client.bottom - 1);
	} else if (!client.contains(cursor)) {
		return Zone::Client;
	}
	if (!request.barVisible || request.bar.bar.empty()) {
		return Zone::Client;
	}
	const auto scale = SafeScale(request.scale);
	const auto x = ToLogical(cursor.x - client.left, scale);
	const auto y = ToLogical(cursor.y - client.top, scale);
	const auto &bar = request.bar;
	if (!bar.bar.contains(x, y)) {
		return Zone::Client;
	} else if (bar.close.contains(x, y)) {
		return Zone::Close;
	} else if (bar.maximize.contains(x, y)) {
		return Zone::Maximize;
	} else if (bar.minimize.contains(x, y)) {
		return Zone::Minimize;
	}
	return Zone::Caption;
}

} // namespace Window::TitleHit
