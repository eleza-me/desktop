// Menus.
//
// Ui::PopupMenu — a context menu in its own top-level window:
//
//   const auto menu = new Ui::PopupMenu(this);          // deletes itself when hidden
//   menu->addAction(tr::lng_reply(tr::now), [=] { ... }, &st::menuIconReply);
//   menu->addSeparator();
//   menu->addItem({ .text = tr::lng_delete(tr::now), .callback = ..., .icon = &st::menuIconDelete,
//                   .destructive = true });
//   menu->popup(e->globalPos());                         // at the cursor
//   menu->popupAnchored(button);                         // under a widget, trailing edges aligned
//
// It opens towards the reading direction (right-down in LTR, left-down in RTL), flips to
// stay on screen, supports keyboard navigation (arrows, Enter, Esc) and submenus
// (MenuItem::fillSubmenu).
//
// Ui::DropdownMenu — the same list as a child widget inside the window (no separate
// native window; used for "more" buttons of top bars so it can be animated and grabbed):
//
//   const auto menu = Ui::CreateChild<Ui::DropdownMenu>(window->bodyWidget());
//   menu->addAction(...);
//   menu->showAnchored(button);    // hides on outside click / Esc / action
#pragma once

#include "ui/widgets/menu.h"
#include "ui/effects/animations.h"

#include <QtCore/QBasicTimer>
#include <QtCore/QPointer>

namespace Ui {

class PopupMenu : public RpWidget {
	Q_OBJECT

public:
	explicit PopupMenu(
		QWidget *parent,
		const style::PopupMenu &st = st::defaultPopupMenu);
	~PopupMenu();

	int addItem(MenuItem item);
	int addAction(
		const QString &text,
		std::function<void()> callback,
		const style::icon *icon = nullptr);
	int addSeparator();
	void clearItems();
	[[nodiscard]] bool empty() const;
	[[nodiscard]] Menu *menu() const {
		return _menu;
	}

	// Keep the object alive after it hides (default: deleteLater on hide).
	void setDestroyOnHide(bool destroy) {
		_destroyOnHide = destroy;
	}

	// A widget shown as its own rounded panel above the list (a strip of quick
	// reactions over a message menu). It gets its natural width; the menu's
	// corner stays at the popup point.
	template <typename Widget>
	Widget *setTopWidget(object_ptr<Widget> widget) {
		const auto result = widget.data();
		doSetTopWidget(Owned<RpWidget>(widget.release()));
		return result;
	}

	// Shows with the top leading corner at the point.
	void popup(const QPoint &globalPosition);
	// Shows under `anchor`. `align`: al_right = trailing edges aligned (default),
	// al_left = leading edges, al_center.
	void popupAnchored(QWidget *anchor, style::align align = style::al_right);
	void hideMenu(bool fast = false);

	// The rendered menu with its shadow (tests, screenshots).
	[[nodiscard]] QPixmap grabForPreview();

Q_SIGNALS:
	void aboutToHide();

protected:
	void paintEvent(QPaintEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	void timerEvent(QTimerEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;

private:
	void prepareGeometry();
	void doSetTopWidget(object_ptr<RpWidget> widget);
	[[nodiscard]] QRect topRect() const;
	[[nodiscard]] QRect menuRect() const;
	void showAt(QPoint topStart, bool startIsRight, QRect avoid = QRect());
	void openSubmenu(int index, bool byKeyboard);
	void closeSubmenu();
	void childTriggered();
	[[nodiscard]] QRect bodyRect() const;

	const style::PopupMenu &_st;
	Menu *_menu = nullptr;
	QPointer<RpWidget> _top;
	PopupMenu *_parentMenu = nullptr;
	QPointer<PopupMenu> _submenu;
	int _submenuIndex = -1;
	int _pendingSubmenu = -1;
	QBasicTimer _submenuTimer;
	bool _destroyOnHide = true;
	bool _hiding = false;
	Animations::Simple _showAnimation;

};

class DropdownMenu : public RpWidget {
	Q_OBJECT

public:
	explicit DropdownMenu(
		QWidget *parent,
		const style::PopupMenu &st = st::defaultPopupMenu);
	~DropdownMenu();

	int addItem(MenuItem item);
	int addAction(
		const QString &text,
		std::function<void()> callback,
		const style::icon *icon = nullptr);
	int addSeparator();
	void clearItems();
	[[nodiscard]] Menu *menu() const {
		return _menu;
	}

	// Positions the menu under the anchor (which may be anywhere inside the same
	// window) and shows it.
	void showAnchored(QWidget *anchor, style::align align = style::al_right);
	// Shows with the top leading corner at a point of the parent.
	void showAt(QPoint parentPosition);
	void hideAnimated();
	void hideFast();
	[[nodiscard]] bool shown() const {
		return _shown;
	}
	void setDestroyOnHide(bool destroy) {
		_destroyOnHide = destroy;
	}

Q_SIGNALS:
	void hidden();

protected:
	void paintEvent(QPaintEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	void prepareGeometry();
	void startShow(QPoint topLeft);
	void animationCallback();
	[[nodiscard]] QRect bodyRect() const;

	const style::PopupMenu &_st;
	Menu *_menu = nullptr;
	bool _shown = false;
	bool _destroyOnHide = false;
	QPixmap _cache;
	Animations::Simple _animation;

};

} // namespace Ui
