#include "window/window_title_bar.h"

#include "lang_auto.h"
#include "ui/painter.h"
#include "ui/widgets/tooltip.h"
#include "styles/style_window.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>

#include <array>

namespace Window {
namespace {

constexpr auto kTooltipDelay = 600;
// Ui::Tooltip::ShowAt() places the bubble below a line of this height.
constexpr auto kTooltipAnchorHeight = 16;

[[nodiscard]] QRect ToQRect(const TitleHit::Rect &rect) {
	return QRect(rect.left, rect.top, rect.width(), rect.height());
}

} // namespace

TitleBar::TitleBar(QWidget *parent) : RpWidget(parent) {
	setAttribute(Qt::WA_OpaquePaintEvent);
	setMouseTracking(true);
	setFocusPolicy(Qt::NoFocus);
	resize(width(), st::titleBarHeight);

	_tooltipTimer.setSingleShot(true);
	_tooltipTimer.setInterval(kTooltipDelay);
	QObject::connect(&_tooltipTimer, &QTimer::timeout, this, [this] {
		showTooltip();
	});
}

TitleBar::~TitleBar() {
	hideTooltip();
}

void TitleBar::setTitle(const QString &title) {
	if (_title != title) {
		_title = title;
		update();
	}
}

void TitleBar::setActive(bool active) {
	if (_active == active) {
		return;
	}
	_active = active;
	if (!active) {
		// Nothing stays hovered or pressed in a window that was left.
		clearState();
	}
	update();
}

void TitleBar::setMaximized(bool maximized) {
	if (_maximized != maximized) {
		_maximized = maximized;
		hideTooltip();
		update();
	}
}

void TitleBar::setDarkOverlay(bool dark) {
	if (_dark != dark) {
		_dark = dark;
		update();
	}
}

void TitleBar::setJoinedWidth(int width) {
	width = std::max(width, 0);
	if (_joinedWidth != width) {
		_joinedWidth = width;
		update();
	}
}

TitleHit::BarLayout TitleBar::layout() const {
	return TitleHit::ComputeBarLayout(
		width(),
		height(),
		st::titleButtonWidth,
		style::RightToLeft());
}

QRect TitleBar::buttonRect(Button button) const {
	const auto layout = this->layout();
	switch (button) {
	case Button::Minimize: return ToQRect(layout.minimize);
	case Button::Maximize: return ToQRect(layout.maximize);
	case Button::Close: return ToQRect(layout.close);
	case Button::None: break;
	}
	return QRect();
}

QRect TitleBar::draggableRect() const {
	return ToQRect(layout().caption);
}

TitleBar::Button TitleBar::buttonAt(QPoint position) const {
	const auto layout = this->layout();
	if (layout.close.contains(position.x(), position.y())) {
		return Button::Close;
	} else if (layout.maximize.contains(position.x(), position.y())) {
		return Button::Maximize;
	} else if (layout.minimize.contains(position.x(), position.y())) {
		return Button::Minimize;
	}
	return Button::None;
}

QColor TitleBar::backgroundColor() const {
	return _dark ? st::titleBarBgDark->c : st::titleBarBg->c;
}

QColor TitleBar::borderColor() const {
	return _dark ? st::titleBarBgDark->c : st::titleWindowBorderFg->c;
}

void TitleBar::updateButton(Button button) {
	if (button != Button::None) {
		update(buttonRect(button));
	}
}

void TitleBar::setOver(Button button) {
	const auto was = overButton();
	_over = button;
	overChanged(was, true);
}

void TitleBar::setNativeOver(Button button, bool withTooltip) {
	const auto was = overButton();
	_nativeOver = button;
	overChanged(was, withTooltip);
}

void TitleBar::overChanged(Button was, bool withTooltip) {
	const auto now = overButton();
	if (was == now) {
		return;
	}
	updateButton(was);
	updateButton(now);
	hideTooltip();
	if (now != Button::None && withTooltip) {
		_tooltipTimer.start();
	}
}

void TitleBar::setNativePressed(Button button) {
	if (_nativeDown == button) {
		return;
	}
	const auto was = std::exchange(_nativeDown, button);
	updateButton(was);
	updateButton(button);
	if (button != Button::None) {
		hideTooltip();
	}
}

void TitleBar::nativeClick(Button button) {
	setNativePressed(Button::None);
	activate(button);
}

void TitleBar::clearState() {
	const auto over = overButton();
	const auto down = downButton();
	_over = _down = _nativeOver = _nativeDown = Button::None;
	_dragPressed = false;
	hideTooltip();
	updateButton(over);
	updateButton(down);
}

void TitleBar::activate(Button button) {
	hideTooltip();
	switch (button) {
	case Button::Minimize: emit minimizeRequested(); break;
	case Button::Maximize: emit maximizeToggleRequested(); break;
	case Button::Close: emit closeRequested(); break;
	case Button::None: break;
	}
}

QString TitleBar::tooltipText(Button button) const {
	switch (button) {
	case Button::Minimize: return tr::lng_title_minimize(tr::now);
	case Button::Maximize:
		return _maximized
			? tr::lng_title_restore(tr::now)
			: tr::lng_title_maximize(tr::now);
	case Button::Close: return tr::lng_title_close(tr::now);
	case Button::None: break;
	}
	return QString();
}

void TitleBar::showTooltip() {
	const auto button = overButton();
	if (button == Button::None
		|| downButton() != Button::None
		|| !isVisible()
		|| Ui::HoverTooltipsSuppressed()) {
		return;
	}
	const auto rect = buttonRect(button);
	Ui::Tooltip::ShowAt(
		mapToGlobal(QPoint(
			rect.x() + rect.width() / 2,
			rect.y() + rect.height() - kTooltipAnchorHeight)),
		tooltipText(button));
	_tooltipShown = true;
}

void TitleBar::hideTooltip() {
	_tooltipTimer.stop();
	if (std::exchange(_tooltipShown, false)) {
		Ui::Tooltip::Hide();
	}
}

void TitleBar::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), _dark ? st::titleBarBgDark : st::titleBarBg);

	const auto layout = this->layout();
	if (!_dark && st::titleBarLineWidth > 0) {
		// The separator from the content; the nav rail below the leading
		// end is of the bar's own surface and joins it without a line.
		const auto joined = std::min(_joinedWidth, width());
		p.fillRect(
			myrtlrect(
				joined,
				height() - st::titleBarLineWidth,
				width() - joined,
				st::titleBarLineWidth),
			st::titleBarLineFg);
	}

	if (!_title.isEmpty()) {
		const auto &font = st::titleBarFont;
		const auto caption = ToQRect(layout.caption);
		const auto available = caption.width() - 2 * st::titleBarTextPadding;
		if (available > 0) {
			const auto full = font->width(_title);
			const auto text = (full > available)
				? font->elided(_title, available)
				: _title;
			const auto textWidth = (full > available)
				? font->width(text)
				: full;
			auto color = _dark ? st::titleBarFgDark->c : st::titleBarFg->c;
			if (!_active) {
				color = style::WithOpacity(color, st::titleBarInactiveOpacity);
			}
			p.setFont(font);
			p.setPen(color);
			const auto left = layout.titleFromRight
				? (caption.x() + caption.width()
					- st::titleBarTextPadding
					- textWidth)
				: (caption.x() + st::titleBarTextPadding);
			p.drawText(
				left,
				(height() - font->height) / 2 + font->ascent,
				text);
		}
	}

	for (const auto button : std::array{
			Button::Minimize,
			Button::Maximize,
			Button::Close }) {
		paintButton(p, button);
	}
}

void TitleBar::paintButton(QPainter &p, Button button) const {
	const auto rect = buttonRect(button);
	if (rect.isEmpty()) {
		return;
	}
	const auto over = (overButton() == button);
	const auto down = over && (downButton() == button);
	const auto close = (button == Button::Close);
	auto color = _dark ? st::titleButtonFgDark->c : st::titleButtonFg->c;
	if (over && close) {
		p.fillRect(rect, down ? st::titleCloseBgDown : st::titleCloseBgOver);
		color = st::titleCloseFg->c;
	} else if (over) {
		p.fillRect(
			rect,
			_dark ? st::titleButtonBgOverDark : st::titleButtonBgOver);
		if (down) {
			// Pressed: one more layer of the same kind.
			p.fillRect(
				rect,
				_dark ? st::titleButtonBgOverDark : st::titleButtonBgDown);
		}
	} else if (!_active) {
		color = style::WithOpacity(color, st::titleBarInactiveOpacity);
	}
	paintGlyph(p, button, rect, color);
}

// The glyphs are drawn in device pixels: one pixel thick lines at every scale
// factor, on whole pixels, so nothing is smeared by a fractional scale or by
// where the bar happens to lie in the window.
void TitleBar::paintGlyph(
		QPainter &p,
		Button button,
		const QRect &rect,
		const QColor &color) const {
	const auto device = p.deviceTransform();
	const auto ratio = device.m11();
	if (device.type() > QTransform::TxScale || ratio <= 0.) {
		return;
	}
	const auto topLeft = device.map(QPointF(rect.x(), rect.y()));
	const auto bottomRight = device.map(QPointF(
		rect.x() + rect.width(),
		rect.y() + rect.height()));
	const auto outer = QRect(
		QPoint(qRound(topLeft.x()), qRound(topLeft.y())),
		QPoint(qRound(bottomRight.x()) - 1, qRound(bottomRight.y()) - 1));
	const auto size = qRound(st::titleButtonGlyphSize * ratio);
	if (size < 4 || outer.width() < size || outer.height() < size) {
		return;
	}
	const auto x = outer.x() + (outer.width() - size) / 2;
	const auto y = outer.y() + (outer.height() - size) / 2;

	p.save();
	// From here on a unit is a device pixel.
	p.setWorldTransform(device.inverted() * p.worldTransform());
	p.setRenderHint(QPainter::Antialiasing, false);
	const auto line = [&](int left, int top, int width, int height) {
		p.fillRect(QRect(left, top, width, height), color);
	};
	const auto frame = [&](int left, int top, int side) {
		line(left, top, side, 1);
		line(left, top + side - 1, side, 1);
		line(left, top + 1, 1, side - 2);
		line(left + side - 1, top + 1, 1, side - 2);
	};
	switch (button) {
	case Button::Minimize:
		line(x, y + size / 2, size, 1);
		break;
	case Button::Maximize:
		if (!_maximized) {
			frame(x, y, size);
			break;
		}
		{
			// Restore: a window in front of another one.
			const auto shift = std::max(qRound(2 * ratio), 2);
			const auto side = size - shift;
			frame(x, y + shift, side);
			line(x + shift, y, side, 1);
			line(x + size - 1, y + 1, 1, side - 1);
			line(x + shift, y + 1, 1, shift - 1);
			line(x + side, y + side - 1, shift - 1, 1);
		}
		break;
	case Button::Close: {
		p.setRenderHint(QPainter::Antialiasing, true);
		auto pen = QPen(color);
		pen.setWidthF(st::titleCloseGlyphStroke);
		pen.setCapStyle(Qt::FlatCap);
		p.setPen(pen);
		p.drawLine(
			QPointF(x, y),
			QPointF(x + size, y + size));
		p.drawLine(
			QPointF(x + size, y),
			QPointF(x, y + size));
	} break;
	case Button::None:
		break;
	}
	p.restore();
}

void TitleBar::mouseMoveEvent(QMouseEvent *e) {
	setOver(buttonAt(e->pos()));
	if (_dragPressed && (e->buttons() & Qt::LeftButton)) {
		const auto delta = e->globalPosition().toPoint() - _dragStart;
		if (delta.manhattanLength() >= QApplication::startDragDistance()) {
			_dragPressed = false;
			if (const auto handle = window()->windowHandle()) {
				handle->startSystemMove();
			}
		}
	}
}

void TitleBar::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	hideTooltip();
	const auto button = buttonAt(e->pos());
	if (button != Button::None) {
		_down = button;
		updateButton(button);
	} else if (draggableRect().contains(e->pos())) {
		// The move starts with the first motion: a press that turns out to
		// be a double click must stay ours.
		_dragPressed = true;
		_dragStart = e->globalPosition().toPoint();
	}
}

void TitleBar::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_dragPressed = false;
	const auto button = std::exchange(_down, Button::None);
	if (button == Button::None) {
		return;
	}
	updateButton(button);
	if (buttonAt(e->pos()) == button) {
		activate(button);
	}
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto button = buttonAt(e->pos());
	if (button != Button::None) {
		// The second press of a fast double click on a button.
		_down = button;
		updateButton(button);
	} else if (draggableRect().contains(e->pos())) {
		_dragPressed = false;
		emit maximizeToggleRequested();
	}
}

void TitleBar::leaveEvent(QEvent *e) {
	setOver(Button::None);
	RpWidget::leaveEvent(e);
}

void TitleBar::hideEvent(QHideEvent *e) {
	clearState();
	RpWidget::hideEvent(e);
}

} // namespace Window
