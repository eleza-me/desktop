// Ui::ListWidget — a virtualised list: rows of variable height, only the visible ones
// are painted. Put it into a Ui::ScrollArea. Chat lists, feeds, member lists and menus
// of settings use it with their own delegate.
//
//   class ChatsDelegate : public Ui::ListDelegate {
//       int rowsCount() const override { return int(_rows.size()); }
//       int rowHeight(int index, int width) const override { return st::dialogsRowHeight; }
//       void paintRow(Painter &p, int index, const QRect &rect, const Ui::ListRowState &state) override;
//   };
//   const auto list = scroll->setOwnedWidget(Ui::object_ptr<Ui::ListWidget>(scroll, &_delegate));
//   connect(list, &Ui::ListWidget::rowClicked, ...);          // left click / Enter
//   connect(list, &Ui::ListWidget::rowContextMenu, ...);      // right click -> Ui::PopupMenu
//   list->refresh();                // after the data changed
//   list->updateRow(i);             // repaint one row; rowHeightChanged(i) if its height changed
//   list->setActiveRow(i);          // the opened chat
//
// paintRow gets `rect` in the list's physical coordinates; use rect.width() as the
// outer width for RTL-aware helpers after p.translate(rect.topLeft()) if you prefer.
// The hover / active background and the press ripple are painted by the list.
#pragma once

#include "ui/rp_widget.h"
#include "ui/effects/ripple_animation.h"
#include "styles/style_widgets.h"

#include <memory>
#include <vector>

class Painter;

namespace Ui {

struct ListRowState {
	bool over = false; // mouse hover or keyboard selection
	bool pressed = false;
	bool active = false; // setActiveRow
};

class ListDelegate {
public:
	[[nodiscard]] virtual int rowsCount() const = 0;
	[[nodiscard]] virtual int rowHeight(int index, int width) const = 0;
	virtual void paintRow(
		Painter &p,
		int index,
		const QRect &rect,
		const ListRowState &state) = 0;
	// Rows that do not react to the mouse (headers, separators).
	[[nodiscard]] virtual bool rowClickable(int index) const {
		Q_UNUSED(index);
		return true;
	}

protected:
	~ListDelegate() = default;

};

class ListWidget : public RpWidget {
	Q_OBJECT

public:
	ListWidget(
		QWidget *parent,
		ListDelegate *delegate,
		const style::ListWidget &st = st::defaultListWidget);
	~ListWidget();

	// Re-reads the rows count and all heights.
	void refresh();
	void updateRow(int index);
	void rowHeightChanged(int index);

	void setActiveRow(int index);
	[[nodiscard]] int activeRow() const {
		return _active;
	}

	// Keyboard selection.
	void selectSkip(int direction);
	void selectSkipPage(int height, int direction);
	void clearSelection();
	[[nodiscard]] int selectedRow() const {
		return _selected;
	}
	bool chooseSelected(); // emits rowClicked for the selection

	[[nodiscard]] int rowAt(QPoint position) const;
	[[nodiscard]] QRect rowRect(int index) const;
	[[nodiscard]] int rowsCount() const {
		return int(_tops.size());
	}
	// Indexes of the first / last rows inside the visible part, -1 if none.
	[[nodiscard]] int firstVisibleRow() const;
	[[nodiscard]] int lastVisibleRow() const;

	// Space above / below the rows (a header, bottom padding).
	void setSkips(int top, int bottom);

Q_SIGNALS:
	void rowClicked(
		int index,
		Qt::MouseButton button,
		Qt::KeyboardModifiers modifiers);
	void rowContextMenu(int index, QPoint globalPosition);
	// The keyboard selection moved: scroll to make [top, bottom) visible.
	void scrollToRequested(int top, int bottom);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override;

private:
	void recount(int width);
	void setOver(int index);
	void setSelected(int index);
	[[nodiscard]] int rowHeightCached(int index) const;

	ListDelegate *_delegate = nullptr;
	const style::ListWidget &_st;
	std::vector<int> _tops; // top of every row
	int _fullHeight = 0;
	int _countedWidth = -1;
	int _topSkip = 0;
	int _bottomSkip = 0;
	int _visibleTop = 0;
	int _visibleBottom = 0;

	int _over = -1;
	int _pressed = -1;
	int _selected = -1;
	int _active = -1;
	bool _mouseSelection = false;
	std::unique_ptr<RippleAnimation> _ripple;
	int _rippleRow = -1;

};

} // namespace Ui
