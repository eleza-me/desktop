#include "ui/widgets/popup_menu.h"

#include "ui/effects/shadow.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"

#include <QtCore/QTimerEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QScreen>
#include <QtWidgets/QApplication>

#include <algorithm>

namespace Ui {
namespace {

constexpr auto kSubmenuDelay = 220;
constexpr auto kScreenPadding = 4;

void PaintPanel(
		QPainter &p,
		const QRect &body,
		const style::PopupMenu &st,
		double opacity = 1.) {
	Shadow::Paint(p, body, st.radius, st.shadow, opacity);
	auto hq = PainterHighQualityEnabler(p);
	p.setOpacity(opacity);
	p.setPen(Qt::NoPen);
	p.setBrush(st.bg);
	p.drawRoundedRect(body, st.radius, st.radius);
	if (st.borderFg->c.alpha() > 0) {
		p.setPen(QPen(st.borderFg->c, 1.));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(
			QRectF(body).marginsRemoved(QMarginsF(0.5, 0.5, 0.5, 0.5)),
			st.radius - 0.5,
			st.radius - 0.5);
	}
	p.setOpacity(1.);
}

[[nodiscard]] QRect ScreenRectFor(const QPoint &global) {
	auto screen = QGuiApplication::screenAt(global);
	if (!screen) {
		screen = QGuiApplication::primaryScreen();
	}
	return screen
		? screen->availableGeometry()
		: QRect(0, 0, 1920, 1080);
}

} // namespace

PopupMenu::PopupMenu(QWidget *parent, const style::PopupMenu &st)
: RpWidget(parent)
, _st(st)
, _menu(new Menu(this, st.menu)) {
	setWindowFlags(Qt::Popup
		| Qt::FramelessWindowHint
		| Qt::NoDropShadowWindowHint);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_NoSystemBackground);
	setMouseTracking(true);
	_parentMenu = qobject_cast<PopupMenu*>(parent);

	QObject::connect(_menu, &Menu::triggered, this, [this] {
		if (_parentMenu) {
			_parentMenu->childTriggered();
		}
		hideMenu(true);
	});
	QObject::connect(_menu, &Menu::submenuRequested, this, [this](int index) {
		openSubmenu(index, false);
	});
	QObject::connect(
		_menu,
		&Menu::selectionChanged,
		this,
		[this](int index, bool byKeyboard) {
			if (byKeyboard) {
				return;
			}
			_pendingSubmenu = index;
			_submenuTimer.start(kSubmenuDelay, this);
		});
}

PopupMenu::~PopupMenu() = default;

int PopupMenu::addItem(MenuItem item) {
	return _menu->addItem(std::move(item));
}

int PopupMenu::addAction(
		const QString &text,
		std::function<void()> callback,
		const style::icon *icon) {
	return _menu->addAction(text, std::move(callback), icon);
}

int PopupMenu::addSeparator() {
	return _menu->addSeparator();
}

void PopupMenu::clearItems() {
	_menu->clearItems();
}

bool PopupMenu::empty() const {
	return _menu->empty();
}

QRect PopupMenu::bodyRect() const {
	return rect().marginsRemoved(_st.shadow.extend);
}

void PopupMenu::prepareGeometry() {
	const auto &extend = _st.shadow.extend;
	_menu->resizeToWidth(_menu->naturalWidth());
	resize(
		extend.left() + _menu->width() + extend.right(),
		extend.top() + _menu->height() + extend.bottom());
	_menu->move(extend.left(), extend.top());
}

void PopupMenu::showAt(QPoint topStart, bool startIsRight, QRect avoid) {
	if (_menu->empty()) {
		if (_destroyOnHide) {
			deleteLater();
		}
		return;
	}
	prepareGeometry();
	const auto &extend = _st.shadow.extend;
	const auto screen = ScreenRectFor(topStart).marginsRemoved(QMargins(
		kScreenPadding,
		kScreenPadding,
		kScreenPadding,
		kScreenPadding));
	const auto bodyWidth = _menu->width();
	const auto bodyHeight = _menu->height();
	auto left = startIsRight ? (topStart.x() - bodyWidth) : topStart.x();
	auto top = topStart.y();
	// Flip to the other side instead of leaving the screen.
	if (left < screen.left()) {
		const auto flipped = avoid.isNull() ? topStart.x() : avoid.right() + 1;
		left = (flipped + bodyWidth <= screen.right() + 1)
			? flipped
			: screen.left();
	} else if (left + bodyWidth > screen.right() + 1) {
		const auto flipped = (avoid.isNull() ? topStart.x() : avoid.left())
			- bodyWidth;
		left = (flipped >= screen.left())
			? flipped
			: (screen.right() + 1 - bodyWidth);
	}
	if (top + bodyHeight > screen.bottom() + 1) {
		const auto flipped = (avoid.isNull() ? topStart.y() : avoid.top())
			- bodyHeight;
		top = (flipped >= screen.top())
			? flipped
			: std::max(screen.bottom() + 1 - bodyHeight, screen.top());
	}
	move(left - extend.left(), top - extend.top());

	_hiding = false;
	setWindowOpacity(AnimationsDisabled() ? 1. : 0.);
	show();
	raise();
	activateWindow();
	setFocus();
	_showAnimation.start([this] {
		setWindowOpacity(_showAnimation.value(1.));
	}, 0., 1., _st.duration, anim::easeOutCubic);
	if (!_showAnimation.animating()) {
		setWindowOpacity(1.);
	}
}

void PopupMenu::popup(const QPoint &globalPosition) {
	showAt(globalPosition, style::RightToLeft());
}

void PopupMenu::popupAnchored(QWidget *anchor, style::align align) {
	if (!anchor) {
		popup(QCursor::pos());
		return;
	}
	const auto global = QRect(
		anchor->mapToGlobal(QPoint()),
		anchor->size());
	const auto skip = st::spaceXs;
	const auto avoid = global.marginsAdded(QMargins(0, skip, 0, skip));
	if (align & Qt::AlignHCenter) {
		prepareGeometry();
		showAt(
			QPoint(global.center().x() - _menu->width() / 2, avoid.bottom() + 1),
			false,
			avoid);
		return;
	}
	// al_right = trailing edges aligned.
	const auto trailing = (align & Qt::AlignRight) != 0;
	const auto useRightEdge = (trailing != style::RightToLeft());
	showAt(
		QPoint(
			useRightEdge ? (global.right() + 1) : global.left(),
			avoid.bottom() + 1),
		useRightEdge,
		QRect(
			QPoint(
				useRightEdge ? global.right() + 1 : global.left(),
				avoid.top()),
			QSize(0, avoid.height())));
}

void PopupMenu::hideMenu(bool fast) {
	Q_UNUSED(fast);
	if (_hiding) {
		return;
	}
	_hiding = true;
	closeSubmenu();
	hide();
}

QPixmap PopupMenu::grabForPreview() {
	prepareGeometry();
	setAttribute(Qt::WA_DontShowOnScreen);
	auto result = GrabWidget(this);
	setAttribute(Qt::WA_DontShowOnScreen, false);
	return result;
}

void PopupMenu::openSubmenu(int index, bool byKeyboard) {
	if (index < 0 || index >= _menu->itemsCount()) {
		return;
	}
	const auto &item = _menu->item(index);
	if (!item.fillSubmenu) {
		return;
	} else if (_submenu && _submenuIndex == index) {
		return;
	}
	closeSubmenu();
	const auto submenu = new PopupMenu(this, _st);
	submenu->_parentMenu = this;
	item.fillSubmenu(submenu);
	if (submenu->empty()) {
		delete submenu;
		return;
	}
	_submenu = submenu;
	_submenuIndex = index;
	_menu->setChildShown(index);
	QObject::connect(submenu, &PopupMenu::aboutToHide, this, [this, submenu] {
		if (_submenu == submenu) {
			_submenu = nullptr;
			_submenuIndex = -1;
			_menu->setChildShown(-1);
		}
	});
	const auto itemRect = _menu->itemRect(index);
	const auto body = QRect(
		_menu->mapToGlobal(QPoint()),
		_menu->size());
	const auto overlap = st::spaceXs;
	const auto toLeft = style::RightToLeft();
	submenu->showAt(
		QPoint(
			toLeft ? (body.left() + overlap) : (body.right() + 1 - overlap),
			body.top() + itemRect.top() - _st.menu.skip),
		toLeft,
		body.marginsRemoved(QMargins(overlap, 0, overlap, 0)));
	if (byKeyboard) {
		submenu->menu()->selectNext();
	}
}

void PopupMenu::closeSubmenu() {
	if (const auto submenu = _submenu.data()) {
		_submenu = nullptr;
		_submenuIndex = -1;
		_menu->setChildShown(-1);
		submenu->hideMenu(true);
	}
}

void PopupMenu::childTriggered() {
	if (_parentMenu) {
		_parentMenu->childTriggered();
	}
	hideMenu(true);
}

void PopupMenu::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = QPainter(this);
	p.setCompositionMode(QPainter::CompositionMode_Source);
	p.fillRect(rect(), Qt::transparent);
	p.setCompositionMode(QPainter::CompositionMode_SourceOver);
	PaintPanel(p, bodyRect(), _st);
}

void PopupMenu::keyPressEvent(QKeyEvent *e) {
	const auto forward = style::RightToLeft() ? Qt::Key_Left : Qt::Key_Right;
	const auto backward = style::RightToLeft() ? Qt::Key_Right : Qt::Key_Left;
	const auto key = e->key();
	if (key == Qt::Key_Down) {
		_menu->selectNext();
	} else if (key == Qt::Key_Up) {
		_menu->selectPrevious();
	} else if (key == Qt::Key_Return
		|| key == Qt::Key_Enter
		|| key == Qt::Key_Space) {
		const auto selected = _menu->selected();
		if (selected >= 0 && _menu->item(selected).fillSubmenu) {
			openSubmenu(selected, true);
		} else {
			_menu->activateSelected();
		}
	} else if (key == forward) {
		const auto selected = _menu->selected();
		if (selected >= 0 && _menu->item(selected).fillSubmenu) {
			openSubmenu(selected, true);
		}
	} else if (key == backward) {
		if (_parentMenu) {
			hideMenu(true);
		}
	} else if (key == Qt::Key_Escape) {
		hideMenu();
	} else {
		RpWidget::keyPressEvent(e);
	}
}

void PopupMenu::mousePressEvent(QMouseEvent *e) {
	// A press on the shadow area (inside the window, outside the body).
	if (!bodyRect().contains(e->pos())) {
		if (_parentMenu) {
			// Let the parent handle it as its own press.
			const auto global = e->globalPosition().toPoint();
			const auto parentBody = QRect(
				_parentMenu->_menu->mapToGlobal(QPoint()),
				_parentMenu->_menu->size());
			if (parentBody.contains(global)) {
				hideMenu(true);
				return;
			}
			_parentMenu->childTriggered();
		}
		hideMenu();
	}
}

void PopupMenu::hideEvent(QHideEvent *e) {
	_submenuTimer.stop();
	closeSubmenu();
	_showAnimation.stop();
	emit aboutToHide();
	if (_destroyOnHide) {
		deleteLater();
	}
	RpWidget::hideEvent(e);
}

void PopupMenu::timerEvent(QTimerEvent *e) {
	if (e->timerId() == _submenuTimer.timerId()) {
		_submenuTimer.stop();
		const auto index = _pendingSubmenu;
		if (index >= 0
			&& index < _menu->itemsCount()
			&& _menu->item(index).fillSubmenu) {
			openSubmenu(index, false);
		} else if (index >= 0 && _submenu) {
			closeSubmenu();
		}
	} else {
		RpWidget::timerEvent(e);
	}
}

void PopupMenu::mouseMoveEvent(QMouseEvent *e) {
	// While a submenu is the active popup Qt sends it all mouse moves, also
	// those over the parent menu: forward them so hovering another item works.
	if (!_parentMenu || rect().contains(e->pos())) {
		return;
	}
	const auto global = e->globalPosition().toPoint();
	const auto parentMenu = _parentMenu->_menu;
	const auto local = parentMenu->mapFromGlobal(global);
	if (parentMenu->rect().contains(local)) {
		auto forwarded = QMouseEvent(
			QEvent::MouseMove,
			QPointF(local),
			QPointF(global),
			Qt::NoButton,
			e->buttons(),
			e->modifiers());
		QCoreApplication::sendEvent(parentMenu, &forwarded);
	}
}

DropdownMenu::DropdownMenu(QWidget *parent, const style::PopupMenu &st)
: RpWidget(parent)
, _st(st)
, _menu(new Menu(this, st.menu)) {
	hide();
	setFocusPolicy(Qt::StrongFocus);
	QObject::connect(_menu, &Menu::triggered, this, [this] {
		hideAnimated();
	});
}

DropdownMenu::~DropdownMenu() {
	if (qApp) {
		qApp->removeEventFilter(this);
	}
}

int DropdownMenu::addItem(MenuItem item) {
	return _menu->addItem(std::move(item));
}

int DropdownMenu::addAction(
		const QString &text,
		std::function<void()> callback,
		const style::icon *icon) {
	return _menu->addAction(text, std::move(callback), icon);
}

int DropdownMenu::addSeparator() {
	return _menu->addSeparator();
}

void DropdownMenu::clearItems() {
	_menu->clearItems();
}

QRect DropdownMenu::bodyRect() const {
	return rect().marginsRemoved(_st.shadow.extend);
}

void DropdownMenu::prepareGeometry() {
	const auto &extend = _st.shadow.extend;
	_menu->resizeToWidth(_menu->naturalWidth());
	resize(
		extend.left() + _menu->width() + extend.right(),
		extend.top() + _menu->height() + extend.bottom());
	_menu->move(extend.left(), extend.top());
}

void DropdownMenu::showAnchored(QWidget *anchor, style::align align) {
	const auto parent = parentWidget();
	if (!anchor || !parent) {
		return;
	}
	prepareGeometry();
	const auto rect = QRect(
		anchor->mapTo(anchor->window(), QPoint())
			- parent->mapTo(parent->window(), QPoint()),
		anchor->size());
	const auto bodyWidth = _menu->width();
	const auto bodyHeight = _menu->height();
	const auto trailing = (align & Qt::AlignRight) != 0;
	const auto useRightEdge = (trailing != style::RightToLeft());
	auto left = (align & Qt::AlignHCenter)
		? (rect.center().x() - bodyWidth / 2)
		: useRightEdge
		? (rect.right() + 1 - bodyWidth)
		: rect.left();
	auto top = rect.bottom() + 1 + st::spaceXs;
	const auto padding = st::spaceSm;
	left = std::clamp(
		left,
		padding,
		std::max(parent->width() - bodyWidth - padding, padding));
	if (top + bodyHeight + padding > parent->height()) {
		const auto above = rect.top() - st::spaceXs - bodyHeight;
		top = (above >= padding)
			? above
			: std::max(parent->height() - bodyHeight - padding, padding);
	}
	startShow(QPoint(left, top));
}

void DropdownMenu::showAt(QPoint parentPosition) {
	prepareGeometry();
	const auto parent = parentWidget();
	const auto bodyWidth = _menu->width();
	const auto bodyHeight = _menu->height();
	const auto padding = st::spaceSm;
	auto left = style::RightToLeft()
		? (parentPosition.x() - bodyWidth)
		: parentPosition.x();
	auto top = parentPosition.y();
	if (parent) {
		left = std::clamp(
			left,
			padding,
			std::max(parent->width() - bodyWidth - padding, padding));
		top = std::clamp(
			top,
			padding,
			std::max(parent->height() - bodyHeight - padding, padding));
	}
	startShow(QPoint(left, top));
}

void DropdownMenu::startShow(QPoint topLeft) {
	const auto &extend = _st.shadow.extend;
	move(topLeft - QPoint(extend.left(), extend.top()));
	raise();
	_shown = true;
	_menu->clearSelection();
	if (qApp) {
		qApp->installEventFilter(this);
	}
	show();
	_menu->show();
	if (AnimationsDisabled()) {
		_cache = QPixmap();
		setFocus();
		return;
	}
	_cache = GrabWidget(this);
	_menu->hide();
	_animation.start(
		[this] { animationCallback(); },
		0.,
		1.,
		_st.duration,
		anim::easeOutCubic);
	setFocus();
	update();
}

void DropdownMenu::hideAnimated() {
	if (!_shown) {
		return;
	}
	_shown = false;
	if (qApp) {
		qApp->removeEventFilter(this);
	}
	if (AnimationsDisabled() || isHidden()) {
		hideFast();
		return;
	}
	if (_cache.isNull()) {
		_cache = GrabWidget(this);
	}
	_menu->hide();
	_animation.start(
		[this] { animationCallback(); },
		_animation.value(1.),
		0.,
		_st.duration,
		anim::linear);
	update();
}

void DropdownMenu::hideFast() {
	_shown = false;
	if (qApp) {
		qApp->removeEventFilter(this);
	}
	_animation.stop();
	_cache = QPixmap();
	const auto wasVisible = !isHidden();
	hide();
	if (wasVisible) {
		emit hidden();
	}
	if (_destroyOnHide) {
		deleteLater();
	}
}

void DropdownMenu::animationCallback() {
	update();
	if (!_animation.animating()) {
		_cache = QPixmap();
		if (_shown) {
			_menu->show();
		} else {
			hideFast();
		}
	}
}

void DropdownMenu::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = QPainter(this);
	if (!_cache.isNull()) {
		const auto progress = _animation.value(_shown ? 1. : 0.);
		p.setOpacity(progress);
		p.drawPixmap(0, int((1. - progress) * -st::spaceSm), _cache);
		return;
	}
	PaintPanel(p, bodyRect(), _st);
}

void DropdownMenu::keyPressEvent(QKeyEvent *e) {
	const auto key = e->key();
	if (key == Qt::Key_Down) {
		_menu->selectNext();
	} else if (key == Qt::Key_Up) {
		_menu->selectPrevious();
	} else if (key == Qt::Key_Return || key == Qt::Key_Enter) {
		_menu->activateSelected();
	} else if (key == Qt::Key_Escape) {
		hideAnimated();
	} else {
		RpWidget::keyPressEvent(e);
	}
}

bool DropdownMenu::eventFilter(QObject *object, QEvent *event) {
	if (_shown && event->type() == QEvent::MouseButtonPress) {
		const auto widget = qobject_cast<QWidget*>(object);
		if (widget && widget->window() == window()) {
			const auto mouse = static_cast<QMouseEvent*>(event);
			const auto local = mapFromGlobal(mouse->globalPosition().toPoint());
			if (!bodyRect().contains(local)) {
				hideAnimated();
			}
		}
	} else if (_shown && event->type() == QEvent::WindowDeactivate
		&& object == window()) {
		hideAnimated();
	}
	return RpWidget::eventFilter(object, event);
}

} // namespace Ui
