// The session layout: nav rail | side column | main column, collapsing to a single
// column in narrow windows. Created by MainWindow::showSession and driven through
// Window::SessionController — feature code does not use this class directly.
#pragma once

#include "ui/rp_widget.h"
#include "ui/effects/slide_animation.h"
#include "window/window_registry.h"
#include "window/window_session_controller.h"

#include <array>
#include <vector>

namespace Ui {
class ServiceLabel;
} // namespace Ui

namespace Window {

class NavRail;
class SectionWidget;

class MainLayout final : public Ui::RpWidget {
public:
	MainLayout(QWidget *parent, SessionController *controller);
	~MainLayout();

	void showTab(Tab tab, anim::type animated = anim::type::normal);
	[[nodiscard]] Tab currentTab() const {
		return _tab;
	}
	void setSideSection(Tab tab, SectionFactory factory);
	[[nodiscard]] SectionWidget *sideSection(Tab tab) const;
	void setTabBadge(Tab tab, int count, bool muted);

	void showMainSection(SectionFactory factory, const SectionShow &options);
	bool showBack();
	void clearMainStack(Tab tab, anim::type animated = anim::type::normal);
	[[nodiscard]] SectionWidget *mainSection() const;
	[[nodiscard]] int mainStackDepth() const;
	[[nodiscard]] bool singleColumn() const {
		return _singleColumn;
	}

	[[nodiscard]] LayoutState saveState() const;
	void restoreState(const LayoutState &state);

	void setInnerFocus();

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	class Resizer;
	class EmptyMain;
	class Transition;

	struct TabState {
		SectionFactory sideFactory; // overrides the registry
		SectionWidget *side = nullptr;
		std::vector<SectionWidget*> stack; // bottom to top, the top one is shown
	};

	[[nodiscard]] TabState &state(Tab tab) {
		return _tabs[int(tab)];
	}
	[[nodiscard]] const TabState &state(Tab tab) const {
		return _tabs[int(tab)];
	}
	void ensureSide(Tab tab);
	void updateGeometries();
	void updateVisibility();
	void updateBackButtons();
	[[nodiscard]] int sideWidthFor(int layoutWidth) const;
	[[nodiscard]] QRect transitionArea() const; // physical
	void runTransition(
		const QRect &area,
		bool back,
		anim::type animated,
		const std::function<void()> &change);
	void resizerDragged(int globalX);
	void resizerFinished();

	SessionController *_controller = nullptr;
	NavRail *_rail = nullptr;
	Resizer *_resizer = nullptr;
	EmptyMain *_empty = nullptr;
	Transition *_transition = nullptr;
	std::array<TabState, kTabsCount> _tabs;
	Tab _tab = Tab::Chats;
	bool _singleColumn = false;
	bool _restoring = false;
	int _sideWidth = 0;
	QRect _sideRect; // logical (leading-side) coordinates
	QRect _mainRect;

};

} // namespace Window
