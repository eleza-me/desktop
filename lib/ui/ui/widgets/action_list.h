// Ui::ActionList — a Ui::ListWidget whose rows have small clickable zones of their
// own (a «follow» button, a «remove» cross, a link in a header) next to the row click.
//
//   class PeopleDelegate : public Ui::ActionListDelegate {
//       int rowActionAt(int index, QPoint point, int width) const override {
//           return followRect(index, width).contains(point) ? kFollow : 0;
//       }
//       void paintRow(Painter &p, int index, const QRect &rect, const Ui::ListRowState &state) override {
//           const auto over = (_list->overAction(index) == kFollow);   // paint the button hovered
//       }
//   };
//   const auto list = scroll->setOwnedWidget(Ui::object_ptr<Ui::ActionList>(scroll, &_delegate));
//   connect(list, &Ui::ActionList::actionClicked, ...);     // (index, action)
//   connect(list, &Ui::ListWidget::rowClicked, ...);         // everywhere else in the row
//
// A press that starts on an action never becomes a row click (and shows no row
// ripple). Rows that are not clickable may still have actions.
#pragma once

#include "ui/widgets/list_widget.h"

namespace Ui {

class ActionListDelegate : public ListDelegate {
public:
	// The action under `point` (row coordinates, physical); 0 = none.
	[[nodiscard]] virtual int rowActionAt(
			int index,
			QPoint point,
			int width) const {
		Q_UNUSED(index);
		Q_UNUSED(point);
		Q_UNUSED(width);
		return 0;
	}

protected:
	~ActionListDelegate() = default;

};

class ActionList : public ListWidget {
	Q_OBJECT

public:
	ActionList(
		QWidget *parent,
		ActionListDelegate *delegate,
		const style::ListWidget &st = st::defaultListWidget);

	// The hovered / pressed action of a row, 0 if none.
	[[nodiscard]] int overAction(int index) const {
		return (index == _actionRow) ? _action : 0;
	}
	[[nodiscard]] int pressedAction(int index) const {
		return (index == _pressedRow) ? _pressedAction : 0;
	}

Q_SIGNALS:
	void actionClicked(int index, int action);

protected:
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;

private:
	void updateAction(QPoint position);
	void setAction(int row, int action);

	ActionListDelegate *_actions = nullptr;
	int _actionRow = -1;
	int _action = 0;
	int _pressedRow = -1;
	int _pressedAction = 0;

};

} // namespace Ui
