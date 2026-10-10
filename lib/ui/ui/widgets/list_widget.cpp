#include "ui/widgets/list_widget.h"

#include "ui/painter.h"

#include <QtGui/QContextMenuEvent>
#include <QtGui/QMouseEvent>

#include <algorithm>

namespace Ui {

ListWidget::ListWidget(
	QWidget *parent,
	ListDelegate *delegate,
	const style::ListWidget &st)
: RpWidget(parent)
, _delegate(delegate)
, _st(st) {
	setMouseTracking(true);
}

ListWidget::~ListWidget() = default;

void ListWidget::recount(int width) {
	const auto count = _delegate->rowsCount();
	_tops.resize(count);
	auto top = _topSkip;
	for (auto i = 0; i != count; ++i) {
		_tops[i] = top;
		top += std::max(_delegate->rowHeight(i, width), 0);
	}
	_fullHeight = top + _bottomSkip;
	_countedWidth = width;
	if (_over >= count) {
		_over = -1;
	}
	if (_selected >= count) {
		_selected = -1;
	}
	if (_pressed >= count) {
		_pressed = -1;
	}
}

int ListWidget::resizeGetHeight(int newWidth) {
	recount(newWidth);
	return _fullHeight;
}

void ListWidget::refresh() {
	_ripple = nullptr;
	_rippleRow = -1;
	_countedWidth = -1;
	resizeToWidth(width());
	update();
}

void ListWidget::updateRow(int index) {
	if (index >= 0 && index < rowsCount()) {
		update(rowRect(index));
	}
}

void ListWidget::rowHeightChanged(int index) {
	if (index < 0 || index >= rowsCount()) {
		return;
	}
	if (_rippleRow >= index) {
		_ripple = nullptr;
		_rippleRow = -1;
	}
	resizeToWidth(width());
	update();
}

void ListWidget::setSkips(int top, int bottom) {
	_topSkip = top;
	_bottomSkip = bottom;
	refresh();
}

int ListWidget::rowHeightCached(int index) const {
	const auto next = (index + 1 < rowsCount())
		? _tops[index + 1]
		: (_fullHeight - _bottomSkip);
	return next - _tops[index];
}

QRect ListWidget::rowRect(int index) const {
	if (index < 0 || index >= rowsCount()) {
		return QRect();
	}
	return QRect(0, _tops[index], width(), rowHeightCached(index));
}

int ListWidget::rowAt(QPoint position) const {
	if (_tops.empty()
		|| position.y() < _topSkip
		|| position.y() >= _fullHeight - _bottomSkip
		|| position.x() < 0
		|| position.x() >= width()) {
		return -1;
	}
	const auto i = std::upper_bound(_tops.begin(), _tops.end(), position.y());
	return int(i - _tops.begin()) - 1;
}

int ListWidget::firstVisibleRow() const {
	if (_tops.empty() || _visibleBottom <= _visibleTop) {
		return -1;
	}
	const auto i = std::upper_bound(_tops.begin(), _tops.end(), _visibleTop);
	return std::max(int(i - _tops.begin()) - 1, 0);
}

int ListWidget::lastVisibleRow() const {
	if (_tops.empty() || _visibleBottom <= _visibleTop) {
		return -1;
	}
	const auto i = std::lower_bound(_tops.begin(), _tops.end(), _visibleBottom);
	return std::clamp(int(i - _tops.begin()) - 1, 0, rowsCount() - 1);
}

void ListWidget::visibleTopBottomUpdated(int visibleTop, int visibleBottom) {
	_visibleTop = visibleTop;
	_visibleBottom = visibleBottom;
}

void ListWidget::setActiveRow(int index) {
	if (_active != index) {
		const auto was = _active;
		_active = index;
		updateRow(was);
		updateRow(index);
	}
}

void ListWidget::setOver(int index) {
	if (index >= 0 && !_delegate->rowClickable(index)) {
		index = -1;
	}
	if (_over != index) {
		const auto was = _over;
		_over = index;
		updateRow(was);
		updateRow(index);
		setCursor((index >= 0) ? style::cur_pointer : style::cur_default);
	}
}

void ListWidget::setSelected(int index) {
	if (_selected != index) {
		const auto was = _selected;
		_selected = index;
		updateRow(was);
		updateRow(index);
	}
}

void ListWidget::selectSkip(int direction) {
	const auto count = rowsCount();
	if (!count || !direction) {
		return;
	}
	_mouseSelection = false;
	auto index = (_selected >= 0)
		? _selected
		: (_over >= 0)
		? _over
		: (direction > 0 ? -1 : count);
	const auto step = (direction > 0) ? 1 : -1;
	auto left = std::abs(direction);
	auto candidate = index;
	while (left > 0) {
		candidate += step;
		if (candidate < 0 || candidate >= count) {
			break;
		}
		if (_delegate->rowClickable(candidate)) {
			index = candidate;
			--left;
		}
	}
	if (index < 0 || index >= count || !_delegate->rowClickable(index)) {
		return;
	}
	setOver(-1);
	setSelected(index);
	const auto rect = rowRect(index);
	emit scrollToRequested(rect.top(), rect.top() + rect.height());
}

void ListWidget::selectSkipPage(int height, int direction) {
	const auto count = rowsCount();
	if (!count || !direction) {
		return;
	}
	const auto step = (direction > 0) ? 1 : -1;
	const auto from = (_selected >= 0) ? _selected : (step > 0 ? 0 : count - 1);
	const auto target = _tops[from] + step * height;
	auto rows = 1;
	for (auto i = from + step; i >= 0 && i < count; i += step) {
		if ((step > 0) ? (_tops[i] > target) : (_tops[i] < target)) {
			break;
		}
		++rows;
	}
	selectSkip(step * rows);
}

void ListWidget::clearSelection() {
	setSelected(-1);
}

bool ListWidget::chooseSelected() {
	if (_selected < 0 || _selected >= rowsCount()) {
		return false;
	}
	emit rowClicked(_selected, Qt::LeftButton, Qt::NoModifier);
	return true;
}

void ListWidget::mouseMoveEvent(QMouseEvent *e) {
	_mouseSelection = true;
	if (_selected >= 0) {
		setSelected(-1);
	}
	setOver(rowAt(e->pos()));
}

void ListWidget::mousePressEvent(QMouseEvent *e) {
	_mouseSelection = true;
	setOver(rowAt(e->pos()));
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_pressed = _over;
	if (_pressed >= 0) {
		const auto rect = rowRect(_pressed);
		if (_rippleRow != _pressed || !_ripple) {
			_rippleRow = _pressed;
			const auto row = _pressed;
			_ripple = std::make_unique<RippleAnimation>(
				_st.ripple,
				RippleAnimation::RectMask(rect.size()),
				[this, row] { updateRow(row); });
		}
		_ripple->add(e->pos() - rect.topLeft());
		updateRow(_pressed);
	}
}

void ListWidget::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (_ripple) {
		_ripple->lastStop();
	}
	updateRow(pressed);
	setOver(rowAt(e->pos()));
	if (e->button() == Qt::LeftButton && pressed >= 0 && pressed == _over) {
		emit rowClicked(pressed, e->button(), e->modifiers());
	}
}

void ListWidget::leaveEvent(QEvent *e) {
	setOver(-1);
	RpWidget::leaveEvent(e);
}

void ListWidget::contextMenuEvent(QContextMenuEvent *e) {
	const auto index = (e->reason() == QContextMenuEvent::Mouse)
		? rowAt(e->pos())
		: _selected;
	if (index < 0 || !_delegate->rowClickable(index)) {
		e->ignore();
		return;
	}
	emit rowContextMenu(
		index,
		(e->reason() == QContextMenuEvent::Mouse)
			? e->globalPos()
			: mapToGlobal(rowRect(index).center()));
}

void ListWidget::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto clip = e->rect();
	if (_st.bg->c.alpha() > 0) {
		p.fillRect(clip, _st.bg);
	}
	if (_tops.empty()) {
		return;
	}
	if (_countedWidth != width()) {
		recount(width());
	}
	const auto from = std::max(
		int(std::upper_bound(_tops.begin(), _tops.end(), clip.top())
			- _tops.begin()) - 1,
		0);
	for (auto i = from; i != rowsCount(); ++i) {
		const auto rect = rowRect(i);
		if (rect.top() >= clip.top() + clip.height()) {
			break;
		} else if (rect.height() <= 0) {
			continue;
		}
		auto state = ListRowState();
		state.over = (i == _over) || (i == _selected);
		state.pressed = (i == _pressed);
		state.active = (i == _active);
		if (state.active) {
			p.fillRect(rect, _st.bgActive);
		} else if (state.over) {
			p.fillRect(rect, _st.bgOver);
		}
		if (_ripple && _rippleRow == i) {
			_ripple->paint(p, rect.x(), rect.y());
			if (_ripple->empty()) {
				_ripple = nullptr;
				_rippleRow = -1;
			}
		}
		p.save();
		p.setClipRect(rect.intersected(clip));
		_delegate->paintRow(p, i, rect, state);
		p.restore();
	}
}

} // namespace Ui
