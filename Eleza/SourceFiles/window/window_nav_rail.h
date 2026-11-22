// The navigation rail: a narrow vertical bar with the tabs (Eli, chats, search, me)
// and settings at the bottom. Sits on the leading side of the window.
// Keyboard: a Tab stop; Up / Down (Home / End) move between the tabs,
// Space / Enter open one. Screen readers see a tab list.
#pragma once

#include "ui/accessibility.h"
#include "ui/rp_widget.h"
#include "ui/effects/animations.h"
#include "ui/effects/ripple_animation.h"
#include "window/window_registry.h"

#include <array>
#include <memory>

namespace Window {

class NavRail final
	: public Ui::RpWidget
	, public Ui::Accessibility::ItemsSource {
	Q_OBJECT

public:
	explicit NavRail(QWidget *parent);
	~NavRail();

	void setActive(Tab tab);
	[[nodiscard]] Tab active() const {
		return _active;
	}
	// count > 0: a number badge; count < 0: a dot; 0: nothing.
	void setBadge(Tab tab, int count, bool muted = false);

	int accessibilityItemsCount() const override;
	Ui::Accessibility::Item accessibilityItem(int index) const override;
	void accessibilityItemPress(int index) override;
	int accessibilityFocusedItem() const override;
	QAccessible::Role accessibilityRole() const override {
		return QAccessible::PageTabList;
	}

Q_SIGNALS:
	void tabChosen(Window::Tab tab);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void focusInEvent(QFocusEvent *e) override;
	void focusOutEvent(QFocusEvent *e) override;

private:
	struct Item {
		Tab tab = Tab::Eli;
		QRect rect;
		int badge = 0;
		bool badgeMuted = false;
		std::unique_ptr<Ui::RippleAnimation> ripple;
	};

	void layoutItems();
	[[nodiscard]] int itemAt(QPoint position) const;
	[[nodiscard]] QString label(Tab tab) const;
	[[nodiscard]] const style::icon &icon(Tab tab, bool active) const;
	[[nodiscard]] QRect pillRect(const Item &item) const;
	void setKeyboardIndex(int index);

	std::array<Item, kTabsCount> _items;
	Tab _active = Tab::Chats;
	int _over = -1;
	int _pressed = -1;
	Ui::Animations::Simple _activeAnimation;
	int _previousActive = -1;
	int _keyboardIndex = -1; // the item the keyboard is on
	bool _keyboardFocus = false;

};

} // namespace Window
