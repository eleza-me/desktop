// Ui::TabBar — a segmented control: text tabs with an animated underline.
//
//   const auto tabs = Ui::CreateChild<Ui::TabBar>(this);
//   tabs->setTabs({ tr::lng_for_you(tr::now), tr::lng_following(tr::now) });
//   connect(tabs, &Ui::TabBar::activeChanged, this, [=](int index) { ... });
//   tabs->resizeToWidth(width());
#pragma once

#include "ui/rp_widget.h"
#include "ui/effects/animations.h"
#include "ui/effects/ripple_animation.h"
#include "styles/style_widgets.h"

#include <memory>
#include <utility>
#include <vector>

namespace Ui {

class TabBar : public RpWidget {
	Q_OBJECT

public:
	explicit TabBar(QWidget *parent, const style::TabBar &st = st::defaultTabBar);
	~TabBar();

	void setTabs(const QStringList &tabs);
	[[nodiscard]] int count() const {
		return int(_tabs.size());
	}
	[[nodiscard]] int active() const {
		return _active;
	}
	void setActive(int index, anim::type animated = anim::type::normal);
	// Equal widths over the whole bar instead of content-sized tabs.
	void setStretch(bool stretch);
	// A dot next to the tab's text (something new inside).
	void setTabBadge(int index, bool shown);

	[[nodiscard]] int naturalWidth() const override;

Q_SIGNALS:
	void activeChanged(int index);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	struct Tab {
		QString text;
		int textWidth = 0;
		int left = 0; // logical
		int width = 0;
		bool badge = false;
		std::unique_ptr<RippleAnimation> ripple;
	};

	void layoutTabs(int forWidth);
	[[nodiscard]] int tabAt(QPoint position) const;
	[[nodiscard]] QRect tabRect(int index) const; // physical
	// Logical left and width of the underline for a tab.
	[[nodiscard]] std::pair<double, double> indicatorTarget(int index) const;

	const style::TabBar &_st;
	std::vector<Tab> _tabs;
	int _active = 0;
	int _over = -1;
	int _pressed = -1;
	bool _stretch = false;
	double _indicatorFrom = 0.;
	double _indicatorWidthFrom = 0.;
	Animations::Simple _indicatorAnimation;

};

} // namespace Ui
