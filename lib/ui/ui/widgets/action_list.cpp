#include "ui/widgets/action_list.h"

#include <QtGui/QMouseEvent>

#include <utility>

namespace Ui {

ActionList::ActionList(
	QWidget *parent,
	ActionListDelegate *delegate,
	const style::ListWidget &st)
: ListWidget(parent, delegate, st)
, _actions(delegate) {
}

void ActionList::setAction(int row, int action) {
	if (!action) {
		row = -1;
	}
	if (_actionRow == row && _action == action) {
		return;
	}
	const auto was = _actionRow;
	_actionRow = row;
	_action = action;
	updateRow(was);
	updateRow(row);
}

void ActionList::updateAction(QPoint position) {
	const auto row = rowAt(position);
	const auto action = (row >= 0)
		? _actions->rowActionAt(
			row,
			position - rowRect(row).topLeft(),
			width())
		: 0;
	const auto had = (_action != 0);
	setAction(row, action);
	if (action) {
		setCursor(style::cur_pointer);
	} else if (had && (row < 0 || !_actions->rowClickable(row))) {
		setCursor(style::cur_default);
	}
}

void ActionList::mouseMoveEvent(QMouseEvent *e) {
	ListWidget::mouseMoveEvent(e);
	updateAction(e->pos());
}

void ActionList::mousePressEvent(QMouseEvent *e) {
	updateAction(e->pos());
	if (_action && e->button() == Qt::LeftButton) {
		_pressedRow = _actionRow;
		_pressedAction = _action;
		updateRow(_pressedRow);
		return;
	}
	ListWidget::mousePressEvent(e);
}

void ActionList::mouseReleaseEvent(QMouseEvent *e) {
	if (_pressedAction) {
		const auto row = std::exchange(_pressedRow, -1);
		const auto action = std::exchange(_pressedAction, 0);
		updateRow(row);
		ListWidget::mouseMoveEvent(e);
		updateAction(e->pos());
		if (e->button() == Qt::LeftButton
			&& row == _actionRow
			&& action == _action) {
			Q_EMIT actionClicked(row, action);
		}
		return;
	}
	ListWidget::mouseReleaseEvent(e);
	updateAction(e->pos());
}

void ActionList::leaveEvent(QEvent *e) {
	setAction(-1, 0);
	ListWidget::leaveEvent(e);
}

} // namespace Ui
