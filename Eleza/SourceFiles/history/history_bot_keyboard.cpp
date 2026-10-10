#include "history/history_bot_keyboard.h"

#include "ui/painter.h"
#include "styles/style_history.h"

#include <QtGui/QMouseEvent>

namespace History {
namespace {

// More rows than this are rare; the rest would push the list off a small
// window, so the keyboard stops growing there.
constexpr auto kMaxRows = 6;
constexpr auto kMaxColumns = 8;

} // namespace

BotKeyboard::BotKeyboard(QWidget *parent) : RpWidget(parent) {
	setMouseTracking(true);
}

void BotKeyboard::setRows(const QVector<QStringList> &rows) {
	if (_rows == rows) {
		return;
	}
	_rows = rows;
	_buttons.clear();
	_hovered = _pressed = -1;
	auto row = 0;
	for (const auto &texts : rows) {
		if (row == kMaxRows) {
			break;
		}
		auto list = QStringList();
		for (const auto &text : texts) {
			if (!text.trimmed().isEmpty() && list.size() < kMaxColumns) {
				list.push_back(text);
			}
		}
		if (list.isEmpty()) {
			continue;
		}
		auto column = 0;
		for (const auto &text : list) {
			auto button = Button();
			button.text = text;
			button.row = row;
			button.column = column++;
			button.columns = int(list.size());
			_buttons.push_back(std::move(button));
		}
		++row;
	}
	setCursor(style::cur_default);
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
}

int BotKeyboard::resizeGetHeight(int newWidth) {
	if (_buttons.empty()) {
		return 0;
	}
	const auto &padding = st::historyBotKeyboardPadding;
	const auto skip = int(st::historyBotKeyboardSkip);
	const auto height = int(st::historyBotKeyboardButtonHeight);
	const auto available = std::max(
		newWidth - padding.left() - padding.right(),
		0);
	auto rows = 0;
	for (auto &button : _buttons) {
		const auto full = available - (button.columns - 1) * skip;
		const auto left = (full * button.column) / button.columns;
		const auto right = (full * (button.column + 1)) / button.columns;
		// Columns go from the leading side, like the text of the chat.
		button.rect = style::rtlrect(
			padding.left() + left + button.column * skip,
			padding.top() + button.row * (height + skip),
			right - left,
			height,
			newWidth);
		rows = std::max(rows, button.row + 1);
	}
	return padding.top()
		+ rows * height
		+ (rows - 1) * skip
		+ padding.bottom();
}

void BotKeyboard::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.setFont(st::historyBotKeyboardFont);
	const auto radius = double(st::historyBotKeyboardRadius);
	const auto textSkip = int(st::historyBotKeyboardTextSkip);
	for (auto i = 0, count = int(_buttons.size()); i != count; ++i) {
		const auto &button = _buttons[i];
		if (!e->rect().intersects(button.rect)) {
			continue;
		}
		p.setPen(Qt::NoPen);
		p.setBrush((i == _pressed)
			? st::historyBotKeyboardBgDown
			: (i == _hovered)
			? st::historyBotKeyboardBgOver
			: st::historyBotKeyboardBg);
		p.drawRoundedRect(button.rect, radius, radius);
		p.setPen(st::historyBotKeyboardFg);
		const auto available = button.rect.width() - 2 * textSkip;
		const auto text = st::historyBotKeyboardFont->elided(
			button.text,
			std::max(available, 0));
		p.drawText(
			button.rect.adjusted(textSkip, 0, -textSkip, 0),
			Qt::AlignCenter | Qt::TextSingleLine,
			text);
	}
}

int BotKeyboard::buttonAt(QPoint position) const {
	for (auto i = 0, count = int(_buttons.size()); i != count; ++i) {
		if (_buttons[i].rect.contains(position)) {
			return i;
		}
	}
	return -1;
}

void BotKeyboard::setHovered(int index) {
	if (_hovered == index) {
		return;
	}
	const auto repaint = [&](int button) {
		if (button >= 0 && button < int(_buttons.size())) {
			update(_buttons[button].rect);
		}
	};
	repaint(_hovered);
	_hovered = index;
	repaint(_hovered);
	setCursor((index >= 0) ? style::cur_pointer : style::cur_default);
}

void BotKeyboard::mouseMoveEvent(QMouseEvent *e) {
	setHovered(buttonAt(e->pos()));
}

void BotKeyboard::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	setHovered(buttonAt(e->pos()));
	_pressed = _hovered;
	if (_pressed >= 0) {
		update(_buttons[_pressed].rect);
	}
}

void BotKeyboard::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto pressed = std::exchange(_pressed, -1);
	const auto over = buttonAt(e->pos());
	setHovered(over);
	if (pressed < 0 || pressed >= int(_buttons.size())) {
		return;
	}
	update(_buttons[pressed].rect);
	if (pressed == over && _pressedCallback) {
		// The callback may replace the rows: keep the text.
		const auto text = _buttons[pressed].text;
		_pressedCallback(text);
	}
}

void BotKeyboard::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	setHovered(-1);
}

} // namespace History
