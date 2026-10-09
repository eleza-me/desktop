// Ui::Menu — the list of actions used inside PopupMenu and DropdownMenu.
// Normally you do not create it directly, see ui/widgets/popup_menu.h.
#pragma once

#include "ui/rp_widget.h"
#include "ui/effects/ripple_animation.h"
#include "styles/style_widgets.h"

#include <functional>
#include <memory>
#include <vector>

namespace Ui {

class PopupMenu;

struct MenuItem {
	QString text;
	std::function<void()> callback;
	const style::icon *icon = nullptr;
	bool destructive = false;
	bool disabled = false;
	bool separator = false;
	bool checked = false; // a tick on the trailing side
	QString shortcut; // hint text on the trailing side
	// Makes the item open a child menu.
	std::function<void(PopupMenu *submenu)> fillSubmenu;
};

class Menu : public RpWidget {
	Q_OBJECT

public:
	explicit Menu(QWidget *parent, const style::Menu &st = st::defaultMenu);
	~Menu();

	int addItem(MenuItem item);
	int addAction(
		const QString &text,
		std::function<void()> callback,
		const style::icon *icon = nullptr);
	int addSeparator();
	void clearItems();
	[[nodiscard]] bool empty() const {
		return _items.empty();
	}
	[[nodiscard]] int itemsCount() const {
		return int(_items.size());
	}
	[[nodiscard]] const MenuItem &item(int index) const {
		return _items[index].data;
	}
	void setItemDisabled(int index, bool disabled);
	void setItemChecked(int index, bool checked);

	[[nodiscard]] int naturalWidth() const override;

	// Keyboard navigation.
	void selectNext();
	void selectPrevious();
	void clearSelection();
	[[nodiscard]] int selected() const {
		return _selected;
	}
	void setSelected(int index, bool byKeyboard = false);
	bool activateSelected(); // false if nothing is selected
	[[nodiscard]] QRect itemRect(int index) const;
	// Keeps the item painted as hovered (its submenu is open).
	void setChildShown(int index);

Q_SIGNALS:
	// After the item's callback was (or will be) invoked.
	void triggered(int index);
	void submenuRequested(int index);
	void selectionChanged(int index, bool byKeyboard);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	struct Entry {
		MenuItem data;
		int top = 0;
		int height = 0;
		std::unique_ptr<RippleAnimation> ripple;
	};

	[[nodiscard]] int indexAt(QPoint position) const;
	[[nodiscard]] bool selectable(int index) const;
	void relayout();
	void trigger(int index);

	const style::Menu &_st;
	std::vector<Entry> _items;
	bool _hasIcons = false;
	int _selected = -1;
	int _pressed = -1;
	int _childShown = -1;

};

} // namespace Ui
