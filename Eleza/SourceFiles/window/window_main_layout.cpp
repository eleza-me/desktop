#include "window/window_main_layout.h"

#include "core/core_application.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "window/window_nav_rail.h"
#include "window/window_placeholder_section.h"
#include "window/window_routes.h"
#include "window/window_section_widget.h"
#include "window/window_update.h"
#include "styles/style_window.h"
#include "styles/style_window_update.h"

#include <QtGui/QMouseEvent>

#include <algorithm>

namespace Window {

// The invisible handle between the side and the main columns.
class MainLayout::Resizer final : public Ui::RpWidget {
public:
	Resizer(
		QWidget *parent,
		std::function<void(int globalX)> dragged,
		std::function<void()> finished)
	: RpWidget(parent)
	, _dragged(std::move(dragged))
	, _finished(std::move(finished)) {
		setCursor(style::cur_sizehor);
	}

protected:
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton) {
			_dragging = true;
		}
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (_dragging) {
			_dragged(e->globalPosition().toPoint().x());
		}
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton && _dragging) {
			_dragging = false;
			_finished();
		}
	}

private:
	std::function<void(int globalX)> _dragged;
	std::function<void()> _finished;
	bool _dragging = false;

};

// The main column when nothing is opened in the tab.
class MainLayout::EmptyMain final : public Ui::RpWidget {
public:
	explicit EmptyMain(QWidget *parent) : RpWidget(parent) {
	}

	void setTab(Tab tab) {
		_wallpaper = (tab == Tab::Chats);
		_text = TabEmptyHint(tab);
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		p.fillRect(
			rect(),
			_wallpaper ? st::mainColumnEmptyBg : st::mainColumnPlainBg);
		if (_text.isEmpty()) {
			return;
		}
		const auto &font = st::serviceLabelFont;
		const auto padding = st::serviceLabelPadding;
		const auto textWidth = font->width(_text);
		const auto pill = QRect(
			(width() - textWidth) / 2 - padding.left(),
			(height() - font->height) / 2 - padding.top(),
			textWidth + padding.left() + padding.right(),
			font->height + padding.top() + padding.bottom());
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(_wallpaper ? st::glassFill : st::bgSunken);
			p.drawRoundedRect(pill, pill.height() / 2., pill.height() / 2.);
		}
		p.setFont(font);
		p.setPen(st::textSecondary);
		p.drawTextCentered(pill, _text);
	}

private:
	bool _wallpaper = true;
	QString _text;

};

// An overlay that plays the slide between two snapshots of a part of the
// layout while the real widgets are already in their new state under it.
class MainLayout::Transition final : public Ui::RpWidget {
public:
	explicit Transition(QWidget *parent) : RpWidget(parent) {
		hide();
	}

	void start(
			QPixmap was,
			QPixmap now,
			bool back,
			std::function<void()> finished) {
		finish();
		_finished = std::move(finished);
		_slide.setSnapshots(std::move(was), std::move(now));
		show();
		raise();
		_slide.start(back, [this] {
			update();
			if (!_slide.animating()) {
				finish();
			}
		}, st::sectionSlideDuration);
		if (!_slide.animating()) {
			finish();
		}
	}

	void finish() {
		_slide.stop();
		hide();
		if (const auto callback = std::exchange(_finished, nullptr)) {
			callback();
		}
	}

	[[nodiscard]] bool running() const {
		return _slide.animating();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = QPainter(this);
		p.fillRect(rect(), st::bgCanvas);
		_slide.paint(p, rect());
	}

private:
	Ui::SlideAnimation _slide;
	std::function<void()> _finished;

};

MainLayout::MainLayout(QWidget *parent, SessionController *controller)
: RpWidget(parent)
, _controller(controller)
, _rail(new NavRail(this))
, _resizer(new Resizer(
	this,
	[this](int globalX) { resizerDragged(globalX); },
	[this] { resizerFinished(); }))
, _empty(new EmptyMain(this))
, _transition(new Transition(this)) {
	_controller->setLayout(this);
	const auto showUpdateDetails = [this] {
		_controller->openRoute(Routes::Settings(QStringLiteral("update")));
	};
	_updateButton = new UpdateRailButton(this, showUpdateDetails);
	_updateBanner = new UpdateBanner(this, showUpdateDetails);
	// Both follow the same checker: one of them tells the layout.
	_updateButton->setOfferChangedCallback([this] {
		updateGeometries();
		updateVisibility();
	});
	_sideWidth = Core::App().settings().sideColumnWidth();
	if (_sideWidth <= 0) {
		_sideWidth = st::sideColumnWidthDefault;
	}
	const auto saved = TabFromId(Core::App().settings().lastTab());
	_tab = saved.value_or(Tab::Chats);
	_rail->setActive(_tab);
	_empty->setTab(_tab);
	QObject::connect(_rail, &NavRail::tabChosen, this, [this](Tab tab) {
		showTab(tab);
	});
	ensureSide(_tab);
	updateVisibility();
}

MainLayout::~MainLayout() {
	_controller->setLayout(nullptr);
}

void MainLayout::ensureSide(Tab tab) {
	auto &entry = state(tab);
	if (entry.side) {
		return;
	}
	auto factory = entry.sideFactory
		? entry.sideFactory
		: RegisteredTabFactory(tab);
	if (!factory) {
		factory = PlaceholderSection::Factory(tab);
	}
	auto created = factory(this, _controller);
	if (!created) {
		created = PlaceholderSection::Factory(tab)(this, _controller);
	}
	entry.side = created.release();
	entry.side->setParent(this);
	entry.side->hide();
}

void MainLayout::setSideSection(Tab tab, SectionFactory factory) {
	auto &entry = state(tab);
	entry.sideFactory = std::move(factory);
	if (const auto was = std::exchange(entry.side, nullptr)) {
		was->hide();
		was->deleteLater();
		if (tab == _tab) {
			ensureSide(tab);
			updateGeometries();
			updateVisibility();
		}
	}
}

SectionWidget *MainLayout::sideSection(Tab tab) const {
	return state(tab).side;
}

void MainLayout::setTabBadge(Tab tab, int count, bool muted) {
	_rail->setBadge(tab, count, muted);
}

SectionWidget *MainLayout::mainSection() const {
	const auto &stack = state(_tab).stack;
	return stack.empty() ? nullptr : stack.back();
}

int MainLayout::mainStackDepth() const {
	return int(state(_tab).stack.size());
}

int MainLayout::sideWidthFor(int layoutWidth) const {
	const auto available = layoutWidth
		- st::navRailWidth
		- st::mainColumnWidthMin;
	return std::clamp(
		std::min(_sideWidth, available),
		st::sideColumnWidthMin,
		st::sideColumnWidthMax);
}

int MainLayout::bannerHeight() const {
	return ((width() < st::layoutSingleColumnWidth)
		&& (_updateButton->offer() != UpdateOffer::None))
		? int(st::updateBannerHeight)
		: 0;
}

void MainLayout::updateGeometries() {
	const auto single = (width() < st::layoutSingleColumnWidth);
	const auto railWidth = st::navRailWidth;
	// The update banner of the single column layout pushes everything down.
	const auto top = bannerHeight();
	const auto bodyHeight = std::max(height() - top, 0);
	if (single) {
		_sideRect = QRect(railWidth, top, std::max(width() - railWidth, 0), bodyHeight);
		_mainRect = QRect(0, top, width(), bodyHeight);
	} else {
		const auto sideWidth = sideWidthFor(width());
		_sideRect = QRect(railWidth, top, sideWidth, bodyHeight);
		const auto mainLeft = railWidth + sideWidth + st::lineWidth;
		_mainRect = QRect(mainLeft, top, std::max(width() - mainLeft, 0), bodyHeight);
	}
	_rail->setGeometryToLeft(0, top, railWidth, bodyHeight, width());
	_updateBanner->setGeometry(0, 0, width(), st::updateBannerHeight);
	// Above «تنظیمات», which the rail pins to its bottom.
	_updateButton->setGeometryToLeft(
		0,
		height()
			- st::navRailPadding.bottom()
			- 2 * st::navRailItemHeight
			- st::navRailItemSkip,
		railWidth,
		st::navRailItemHeight,
		width());
	const auto &entry = state(_tab);
	if (entry.side) {
		entry.side->setGeometryToLeft(_sideRect, width());
	}
	for (const auto section : entry.stack) {
		section->setGeometryToLeft(_mainRect, width());
	}
	_empty->setGeometryToLeft(_mainRect, width());
	const auto handle = st::columnResizerWidth;
	_resizer->setGeometryToLeft(
		_sideRect.x() + _sideRect.width() - handle / 2,
		top,
		handle,
		bodyHeight,
		width());
	if (_singleColumn != single) {
		_singleColumn = single;
		updateVisibility();
		updateBackButtons();
		_controller->notifyColumnsChanged(single);
	}
}

void MainLayout::updateVisibility() {
	const auto hasMain = !state(_tab).stack.empty();
	const auto covered = _singleColumn && hasMain;
	for (auto i = 0; i != kTabsCount; ++i) {
		const auto &entry = _tabs[i];
		const auto current = (Tab(i) == _tab);
		if (entry.side) {
			entry.side->setVisible(current && !covered);
		}
		for (const auto section : entry.stack) {
			section->setVisible(current && section == entry.stack.back());
		}
	}
	_rail->setVisible(!covered);
	const auto offer = (_updateButton->offer() != UpdateOffer::None);
	_updateButton->setVisible(offer && !_singleColumn);
	_updateButton->raise();
	_updateBanner->setVisible(offer && _singleColumn);
	_empty->setVisible(!_singleColumn && !hasMain);
	_resizer->setVisible(!_singleColumn);
	_resizer->raise();
	if (_transition->running()) {
		_transition->raise();
	}
}

void MainLayout::updateBackButtons() {
	const auto &entry = state(_tab);
	if (entry.side) {
		entry.side->setBackButtonShown(false);
	}
	for (auto i = 0; i != int(entry.stack.size()); ++i) {
		entry.stack[i]->setBackButtonShown(_singleColumn || i > 0);
	}
}

QRect MainLayout::transitionArea() const {
	return myrtlrect(_mainRect);
}

void MainLayout::runTransition(
		const QRect &area,
		bool back,
		anim::type animated,
		const std::function<void()> &change) {
	_transition->finish();
	const auto animate = (animated == anim::type::normal)
		&& !_restoring
		&& !Ui::AnimationsDisabled()
		&& isVisible()
		&& !area.isEmpty();
	if (!animate) {
		change();
		if (const auto section = mainSection()) {
			section->showFinished();
		}
		setInnerFocus();
		return;
	}
	auto was = Ui::GrabWidget(this, area, st::bgCanvas->c);
	change();
	auto now = Ui::GrabWidget(this, area, st::bgCanvas->c);
	_transition->setGeometry(area);
	_transition->start(std::move(was), std::move(now), back, [this] {
		if (const auto section = mainSection()) {
			section->showFinished();
		}
		setInnerFocus();
	});
}

LayoutState MainLayout::saveState() const {
	auto result = LayoutState();
	result.tab = _tab;
	for (auto i = 0; i != kTabsCount; ++i) {
		for (const auto section : _tabs[i].stack) {
			const auto route = section->restoreRoute();
			if (!route.isEmpty()) {
				result.stacks[i].push_back(route);
			}
		}
	}
	return result;
}

void MainLayout::restoreState(const LayoutState &state) {
	_restoring = true;
	const auto restore = [&](Tab tab) {
		const auto &routes = state.stacks[int(tab)];
		if (routes.isEmpty()) {
			return;
		}
		if (tab != _tab) {
			showTab(tab, anim::type::instant);
		}
		for (const auto &route : routes) {
			// Routes that open "where the user is" land in this tab.
			if (_tab != tab) {
				showTab(tab, anim::type::instant);
			}
			[[maybe_unused]] const auto opened = _controller->openRoute(route);
		}
	};
	for (auto i = 0; i != kTabsCount; ++i) {
		if (Tab(i) != state.tab) {
			restore(Tab(i));
		}
	}
	restore(state.tab);
	if (_tab != state.tab) {
		showTab(state.tab, anim::type::instant);
	}
	_restoring = false;
}

void MainLayout::showTab(Tab tab, anim::type animated) {
	Q_UNUSED(animated);
	if (tab == _tab) {
		// The active tab pressed again: back to its list.
		if (!state(tab).stack.empty()) {
			clearMainStack(tab);
		}
		return;
	}
	_transition->finish();
	_tab = tab;
	_rail->setActive(tab);
	_empty->setTab(tab);
	ensureSide(tab);
	updateGeometries();
	updateVisibility();
	updateBackButtons();
	setInnerFocus();
	Core::App().settings().setLastTab(TabId(tab));
	_controller->notifyTabChanged(tab);
	_controller->notifyMainSectionChanged();
}

void MainLayout::showMainSection(
		SectionFactory factory,
		const SectionShow &options) {
	if (!factory) {
		return;
	}
	const auto tab = options.tab.value_or(_tab);
	if (tab != _tab) {
		showTab(tab, anim::type::instant);
	}
	auto created = factory(this, _controller);
	if (!created) {
		return;
	}
	const auto section = created.release();
	auto &stack = state(tab).stack;
	const auto key = section->sectionKey();
	if (options.stack
		&& !key.isEmpty()
		&& !stack.empty()
		&& stack.back()->sectionKey() == key) {
		delete section;
		stack.back()->setInnerFocus();
		return;
	}
	section->setParent(this);
	section->hide();
	runTransition(transitionArea(), false, options.animated, [&] {
		if (!options.stack) {
			for (const auto old : stack) {
				old->hide();
				old->deleteLater();
			}
			stack.clear();
		}
		stack.push_back(section);
		updateGeometries();
		updateVisibility();
		updateBackButtons();
	});
	_controller->notifyMainSectionChanged();
}

bool MainLayout::showBack() {
	auto &entry = state(_tab);
	if (entry.stack.empty()) {
		return entry.side ? entry.side->onBackRequest() : false;
	} else if (entry.stack.back()->onBackRequest()) {
		return true;
	}
	runTransition(transitionArea(), true, anim::type::normal, [&] {
		const auto top = entry.stack.back();
		entry.stack.pop_back();
		top->hide();
		top->deleteLater();
		updateGeometries();
		updateVisibility();
		updateBackButtons();
	});
	_controller->notifyMainSectionChanged();
	return true;
}

void MainLayout::clearMainStack(Tab tab, anim::type animated) {
	auto &entry = state(tab);
	if (entry.stack.empty()) {
		return;
	}
	const auto change = [&] {
		for (const auto section : entry.stack) {
			section->hide();
			section->deleteLater();
		}
		entry.stack.clear();
		updateGeometries();
		updateVisibility();
		updateBackButtons();
	};
	if (tab == _tab) {
		runTransition(transitionArea(), true, animated, change);
		_controller->notifyMainSectionChanged();
	} else {
		change();
	}
}

void MainLayout::setInnerFocus() {
	if (const auto section = mainSection()) {
		section->setInnerFocus();
	} else if (const auto side = state(_tab).side) {
		side->setInnerFocus();
	}
}

void MainLayout::resizerDragged(int globalX) {
	const auto local = mapFromGlobal(QPoint(globalX, 0)).x();
	const auto logical = style::RightToLeft() ? (width() - local) : local;
	const auto wanted = std::clamp(
		logical - st::navRailWidth,
		st::sideColumnWidthMin,
		st::sideColumnWidthMax);
	if (_sideWidth != wanted) {
		_sideWidth = wanted;
		updateGeometries();
		update();
	}
}

void MainLayout::resizerFinished() {
	Core::App().settings().setSideColumnWidth(_sideWidth);
}

void MainLayout::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	if (_transition->running()) {
		_transition->finish();
	}
	updateGeometries();
}

void MainLayout::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = QPainter(this);
	p.fillRect(rect(), st::bgCanvas);
	if (!_singleColumn) {
		// The line between the side and the main columns.
		p.fillRect(
			myrtlrect(
				_sideRect.x() + _sideRect.width(),
				0,
				st::lineWidth,
				height()),
			st::columnLineFg);
	}
}

} // namespace Window
