#include "window/window_title_frame.h"

#include "core/core_application.h"
#include "window/window_title_bar.h"
#include "styles/style_window.h"

#include <QtCore/QLibraryInfo>
#include <QtCore/QVersionNumber>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QWindow>
#include <QtWidgets/QWidget>

#include <array>

namespace Window {
namespace {

[[nodiscard]] bool EnvironmentAsksForSystemTitleBar() {
	const auto value = qEnvironmentVariable("ELEZA_SYSTEM_TITLE_BAR");
	return !value.isEmpty() && (value != QStringLiteral("0"));
}

#ifndef Q_OS_WIN

// One of the eight invisible strips around a frameless window: shows the
// resize cursor and hands the drag over to the window system.
class ResizeHandle final : public QWidget {
public:
	ResizeHandle(QWidget *parent, Qt::Edges edges)
	: QWidget(parent)
	, _edges(edges) {
		setFocusPolicy(Qt::NoFocus);
		setCursor(cursorFor(edges));
	}

	[[nodiscard]] Qt::Edges edges() const {
		return _edges;
	}

protected:
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		if (const auto handle = window()->windowHandle()) {
			handle->startSystemResize(_edges);
		}
	}

	// Nobody draws a border around a frameless window: a hairline along
	// the outer edge keeps it apart from what is behind it.
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = QPainter(this);
		const auto line = int(st::lineWidth);
		if (_edges & Qt::LeftEdge) {
			p.fillRect(0, 0, line, height(), st::titleWindowBorderFg);
		}
		if (_edges & Qt::RightEdge) {
			p.fillRect(
				width() - line,
				0,
				line,
				height(),
				st::titleWindowBorderFg);
		}
		if (_edges & Qt::TopEdge) {
			p.fillRect(0, 0, width(), line, st::titleWindowBorderFg);
		}
		if (_edges & Qt::BottomEdge) {
			p.fillRect(
				0,
				height() - line,
				width(),
				line,
				st::titleWindowBorderFg);
		}
	}

private:
	[[nodiscard]] static Qt::CursorShape cursorFor(Qt::Edges edges) {
		if (edges == (Qt::TopEdge | Qt::LeftEdge)
			|| edges == (Qt::BottomEdge | Qt::RightEdge)) {
			return Qt::SizeFDiagCursor;
		} else if (edges == (Qt::TopEdge | Qt::RightEdge)
			|| edges == (Qt::BottomEdge | Qt::LeftEdge)) {
			return Qt::SizeBDiagCursor;
		} else if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
			return Qt::SizeHorCursor;
		}
		return Qt::SizeVerCursor;
	}

	const Qt::Edges _edges;

};

class FramelessFrame final : public TitleFrame {
public:
	explicit FramelessFrame(QWidget *window) : _window(window) {
		window->setWindowFlag(Qt::FramelessWindowHint, true);
		auto index = 0;
		for (const auto edges : std::array<Qt::Edges, 8>{
				Qt::Edges(Qt::LeftEdge),
				Qt::Edges(Qt::RightEdge),
				Qt::Edges(Qt::TopEdge),
				Qt::Edges(Qt::BottomEdge),
				Qt::TopEdge | Qt::LeftEdge,
				Qt::TopEdge | Qt::RightEdge,
				Qt::BottomEdge | Qt::LeftEdge,
				Qt::BottomEdge | Qt::RightEdge }) {
			_handles[index++] = new ResizeHandle(window, edges);
		}
		windowChanged();
	}

	void windowChanged() override {
		// Nothing to resize while the window fills the screen.
		const auto shown = !_window->isMaximized() && !_window->isFullScreen();
		const auto width = _window->width();
		const auto height = _window->height();
		const auto border = int(st::titleResizeBorder);
		const auto corner = 2 * border;
		for (const auto handle : _handles) {
			const auto edges = handle->edges();
			const auto horizontal = bool(edges & (Qt::LeftEdge | Qt::RightEdge));
			const auto vertical = bool(edges & (Qt::TopEdge | Qt::BottomEdge));
			auto rect = QRect();
			if (horizontal && vertical) {
				rect = QRect(
					(edges & Qt::LeftEdge) ? 0 : (width - corner),
					(edges & Qt::TopEdge) ? 0 : (height - corner),
					corner,
					corner);
			} else if (horizontal) {
				rect = QRect(
					(edges & Qt::LeftEdge) ? 0 : (width - border),
					corner,
					border,
					height - 2 * corner);
			} else {
				rect = QRect(
					corner,
					(edges & Qt::TopEdge) ? 0 : (height - border),
					width - 2 * corner,
					border);
			}
			handle->setGeometry(rect);
			handle->setVisible(shown && !rect.isEmpty());
			handle->raise();
		}
		// The corner squares must not swallow more than the border strip
		// along each edge: only an L-shaped part of them takes the mouse.
		for (const auto handle : _handles) {
			const auto edges = handle->edges();
			if (!(edges & (Qt::LeftEdge | Qt::RightEdge))
				|| !(edges & (Qt::TopEdge | Qt::BottomEdge))) {
				continue;
			}
			auto mask = QRegion(
				0,
				(edges & Qt::TopEdge) ? 0 : (corner - border),
				corner,
				border);
			mask += QRect(
				(edges & Qt::LeftEdge) ? 0 : (corner - border),
				0,
				border,
				corner);
			handle->setMask(mask);
		}
	}

private:
	QWidget *_window = nullptr;
	std::array<ResizeHandle*, 8> _handles = {};

};

#endif // !Q_OS_WIN

} // namespace

bool CustomTitleBarSupported() {
	const auto platform = QGuiApplication::platformName();
#if defined Q_OS_WIN
	// The frame relies on how the platform plugin tracks a window whose
	// non-client area was removed; that is what it does from 6.7 on.
	return (platform == QStringLiteral("windows"))
		&& (QLibraryInfo::version() >= QVersionNumber(6, 7, 0));
#elif defined Q_OS_LINUX // Q_OS_WIN
	return (platform == QStringLiteral("xcb"))
		|| platform.startsWith(QStringLiteral("wayland"));
#else // Q_OS_WIN || Q_OS_LINUX
	Q_UNUSED(platform);
	return false;
#endif // Q_OS_WIN || Q_OS_LINUX
}

bool UseCustomTitleBar() {
	const auto &arguments = Core::App().commandLine();
	if (arguments.systemTitleBar || EnvironmentAsksForSystemTitleBar()) {
		return false;
	}
#ifdef ELEZA_DEBUG_TOOLS
	if (arguments.debugTitleBar) {
		// For grabs: also where there is no window system to speak of.
		return true;
	}
#endif // ELEZA_DEBUG_TOOLS
	return CustomTitleBarSupported()
		&& !Core::App().settings().systemTitleBar();
}

#ifndef Q_OS_WIN
std::unique_ptr<TitleFrame> CreateTitleFrame(
		QWidget *window,
		TitleBar *title,
		std::function<void()> lost) {
	Q_UNUSED(title);
	Q_UNUSED(lost);
	if (!window || !CustomTitleBarSupported()) {
		return nullptr;
	}
	return std::make_unique<FramelessFrame>(window);
}
#endif // !Q_OS_WIN

} // namespace Window
