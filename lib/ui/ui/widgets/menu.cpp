#include "ui/widgets/menu.h"

#include "ui/painter.h"
#include "ui/ui_utility.h"

#include <QtGui/QMouseEvent>

#include <algorithm>

namespace Ui {

Menu::Menu(QWidget *parent, const style::Menu &st)
: RpWidget(parent)
, _st(st) {
	setMouseTracking(true);
	resize(_st.widthMin, 2 * _st.skip);
}

Menu::~Menu() = default;

int Menu::addItem(MenuItem item) {
	_hasIcons = _hasIcons || (item.icon != nullptr);
	auto entry = Entry();
	entry.data = std::move(item);
	_items.push_back(std::move(entry));
	relayout();
	return int(_items.size()) - 1;
}

int Menu::addAction(
		const QString &text,
		std::function<void()> callback,
		const style::icon *icon) {
	auto item = MenuItem();
	item.text = text;
	item.callback = std::move(callback);
	item.icon = icon;
	return addItem(std::move(item));
}

int Menu::addSeparator() {
	if (_items.empty() || _items.back().data.separator) {
		return int(_items.size()) - 1;
	}
	auto item = MenuItem();
	item.separator = true;
	return addItem(std::move(item));
}

void Menu::clearItems() {
	_items.clear();
	_hasIcons = false;
	_selected = _pressed = _childShown = -1;
	relayout();
}

void Menu::setItemDisabled(int index, bool disabled) {
	if (index >= 0 && index < int(_items.size())) {
		_items[index].data.disabled = disabled;
		update();
	}
}

void Menu::setItemChecked(int index, bool checked) {
	if (index >= 0 && index < int(_items.size())) {
		_items[index].data.checked = checked;
		update();
	}
}

int Menu::naturalWidth() const {
	auto result = _st.widthMin;
	for (const auto &entry : _items) {
		const auto &item = entry.data;
		if (item.separator) {
			continue;
		}
		auto width = _st.itemPadding.left()
			+ (_hasIcons ? _st.itemIconSkip : 0)
			+ _st.itemFont->width(item.text)
			+ _st.itemPadding.right();
		if (!item.shortcut.isEmpty()) {
			width += st::spaceLg + _st.shortcutFont->width(item.shortcut);
		}
		if (item.fillSubmenu || item.checked) {
			width += st::spaceSm + _st.arrow.width();
		}
		result = std::max(result, width);
	}
	return std::min(result + 2 * _st.itemMargin, _st.widthMax);
}

void Menu::relayout() {
	auto top = _st.skip;
	for (auto &entry : _items) {
		entry.top = top;
		entry.height = entry.data.separator
			? (_st.separatorPadding.top()
				+ st::lineWidth
				+ _st.separatorPadding.bottom())
			: _st.itemHeight;
		entry.ripple = nullptr;
		top += entry.height;
	}
	resize(naturalWidth(), top + _st.skip);
	update();
}

int Menu::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	auto result = 2 * _st.skip;
	for (const auto &entry : _items) {
		result += entry.height;
	}
	return result;
}

bool Menu::selectable(int index) const {
	return index >= 0
		&& index < int(_items.size())
		&& !_items[index].data.separator
		&& !_items[index].data.disabled;
}

int Menu::indexAt(QPoint position) const {
	if (position.x() < 0 || position.x() >= width()) {
		return -1;
	}
	for (auto i = 0; i != int(_items.size()); ++i) {
		const auto &entry = _items[i];
		if (position.y() >= entry.top
			&& position.y() < entry.top + entry.height) {
			return selectable(i) ? i : -1;
		}
	}
	return -1;
}

QRect Menu::itemRect(int index) const {
	if (index < 0 || index >= int(_items.size())) {
		return QRect();
	}
	return QRect(0, _items[index].top, width(), _items[index].height);
}

void Menu::setChildShown(int index) {
	if (_childShown != index) {
		_childShown = index;
		update();
	}
}

void Menu::setSelected(int index, bool byKeyboard) {
	if (!selectable(index)) {
		index = -1;
	}
	if (_selected != index) {
		_selected = index;
		update();
		emit selectionChanged(index, byKeyboard);
	}
}

void Menu::selectNext() {
	const auto count = int(_items.size());
	for (auto step = 1; step <= count; ++step) {
		const auto index = (_selected + step + count) % count;
		if (selectable(index)) {
			setSelected(index, true);
			return;
		}
	}
}

void Menu::selectPrevious() {
	const auto count = int(_items.size());
	const auto from = (_selected < 0) ? 0 : _selected;
	for (auto step = 1; step <= count; ++step) {
		const auto index = (from - step + 2 * count) % count;
		if (selectable(index)) {
			setSelected(index, true);
			return;
		}
	}
}

void Menu::clearSelection() {
	setSelected(-1);
}

bool Menu::activateSelected() {
	if (!selectable(_selected)) {
		return false;
	}
	trigger(_selected);
	return true;
}

void Menu::trigger(int index) {
	if (!selectable(index)) {
		return;
	}
	if (_items[index].data.fillSubmenu) {
		emit submenuRequested(index);
		return;
	}
	const auto guard = QPointer<Menu>(this);
	const auto callback = _items[index].data.callback;
	// Close the menu first: callbacks often open boxes / other menus.
	emit triggered(index);
	if (callback) {
		callback();
	}
	Q_UNUSED(guard);
}

void Menu::mouseMoveEvent(QMouseEvent *e) {
	setSelected(indexAt(e->pos()));
}

void Menu::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto index = indexAt(e->pos());
	setSelected(index);
	_pressed = index;
	if (index >= 0) {
		auto &entry = _items[index];
		if (!entry.ripple) {
			const auto margin = _st.itemMargin;
			entry.ripple = std::make_unique<RippleAnimation>(
				_st.ripple,
				RippleAnimation::RoundRectMask(
					QSize(width() - 2 * margin, entry.height),
					_st.itemRadius),
				[this] { update(); });
		}
		entry.ripple->add(e->pos() - QPoint(_st.itemMargin, entry.top));
	}
}

void Menu::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto pressed = std::exchange(_pressed, -1);
	if (pressed >= 0 && pressed < int(_items.size())) {
		if (const auto ripple = _items[pressed].ripple.get()) {
			ripple->lastStop();
		}
	}
	const auto index = indexAt(e->pos());
	// Also accept a release without a press: press-drag-release from the
	// button that opened the menu.
	if (index >= 0 && (pressed == index || pressed < 0)) {
		trigger(index);
	}
}

void Menu::leaveEvent(QEvent *e) {
	if (_childShown < 0) {
		clearSelection();
	}
	RpWidget::leaveEvent(e);
}

void Menu::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto clip = e->rect();
	const auto margin = _st.itemMargin;
	for (auto i = 0; i != int(_items.size()); ++i) {
		auto &entry = _items[i];
		const auto rect = QRect(0, entry.top, width(), entry.height);
		if (!rect.intersects(clip)) {
			continue;
		}
		const auto &item = entry.data;
		if (item.separator) {
			p.fillRect(
				_st.separatorPadding.left(),
				entry.top + _st.separatorPadding.top(),
				width()
					- _st.separatorPadding.left()
					- _st.separatorPadding.right(),
				st::lineWidth,
				_st.separatorFg);
			continue;
		}
		const auto over = (i == _selected || i == _childShown)
			&& !item.disabled;
		const auto inner = rect.marginsRemoved(QMargins(margin, 0, margin, 0));
		if (over) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(_st.itemBgOver);
			p.drawRoundedRect(inner, _st.itemRadius, _st.itemRadius);
		}
		if (entry.ripple) {
			entry.ripple->paint(p, inner.x(), inner.y());
			if (entry.ripple->empty()) {
				entry.ripple = nullptr;
			}
		}
		if (item.disabled) {
			p.setOpacity(0.45);
		}
		const auto &fg = item.destructive
			? _st.dangerFg
			: over
			? _st.itemFgOver
			: _st.itemFg;
		auto left = _st.itemPadding.left();
		if (item.icon) {
			const auto top = entry.top + (entry.height - item.icon->height()) / 2;
			if (item.destructive) {
				item.icon->paint(p, left, top, width(), _st.dangerFg->c);
			} else {
				item.icon->paint(p, left, top, width());
			}
		}
		if (_hasIcons) {
			left += _st.itemIconSkip;
		}
		auto right = _st.itemPadding.right();
		if (item.fillSubmenu) {
			_st.arrow.paint(
				p,
				width() - right - _st.arrow.width(),
				entry.top + (entry.height - _st.arrow.height()) / 2,
				width());
			right += _st.arrow.width() + st::spaceSm;
		} else if (item.checked) {
			_st.check.paint(
				p,
				width() - right - _st.check.width(),
				entry.top + (entry.height - _st.check.height()) / 2,
				width());
			right += _st.check.width() + st::spaceSm;
		}
		if (!item.shortcut.isEmpty()) {
			p.setFont(_st.shortcutFont);
			p.setPen(_st.shortcutFg);
			const auto shortcutWidth = _st.shortcutFont->width(item.shortcut);
			p.drawTextRight(
				right,
				entry.top + (entry.height - _st.shortcutFont->height) / 2,
				width(),
				item.shortcut,
				shortcutWidth);
			right += shortcutWidth + st::spaceLg;
		}
		const auto available = width() - left - right;
		const auto textWidth = _st.itemFont->width(item.text);
		p.setFont(_st.itemFont);
		p.setPen(fg);
		if (textWidth > available) {
			const auto elided = _st.itemFont->elided(item.text, available);
			p.drawTextLeft(
				left,
				entry.top + (entry.height - _st.itemFont->height) / 2,
				width(),
				elided,
				_st.itemFont->width(elided));
		} else {
			p.drawTextLeft(
				left,
				entry.top + (entry.height - _st.itemFont->height) / 2,
				width(),
				item.text,
				textWidth);
		}
		if (item.disabled) {
			p.setOpacity(1.);
		}
	}
}

} // namespace Ui
