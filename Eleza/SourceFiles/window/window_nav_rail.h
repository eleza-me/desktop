// The navigation rail: a narrow vertical bar with the tabs (Eli, chats, search, me)
// and settings at the bottom. Sits on the leading side of the window.
#pragma once

#include "ui/rp_widget.h"
#include "ui/effects/animations.h"
#include "ui/effects/ripple_animation.h"
#include "window/window_registry.h"

#include <array>
#include <memory>

namespace Window {

class NavRail final : public Ui::RpWidget {
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

Q_SIGNALS:
	void tabChosen(Window::Tab tab);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;

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

	std::array<Item, kTabsCount> _items;
	Tab _active = Tab::Chats;
	int _over = -1;
	int _pressed = -1;
	Ui::Animations::Simple _activeAnimation;
	int _previousActive = -1;

};

} // namespace Window
