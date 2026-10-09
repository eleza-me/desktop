// Ui::RpWidget — the base of every widget in the app.
//
//  * RTL-aware geometry: moveToLeft(x, y) places the widget `x` from the leading side of
//    its parent (left in LTR, right in RTL); moveToRight is the opposite.
//  * resizeToWidth(w): the widget computes its own height (override resizeGetHeight).
//  * visibleTopBottomUpdated(): called by scroll areas so long widgets paint lazily.
//  * Signals for geometry / visibility changes; paintRequest() to paint a plain RpWidget
//    without subclassing.
//
// Widgets never use Qt layouts or stylesheets: they place children in resizeEvent() and
// paint from st:: values in paintEvent().
#pragma once

#include "ui/style/style_core.h"
#include "ui/rtl.h"
#include "ui/object_ptr.h"

#include <QtWidgets/QWidget>

#include <functional>

namespace Ui {

class RpWidget : public QWidget {
	Q_OBJECT

public:
	explicit RpWidget(QWidget *parent = nullptr);

	// ---- RTL-aware placement inside the parent (or inside `outerw`).
	void moveToLeft(int x, int y, int outerw = 0);
	void moveToRight(int x, int y, int outerw = 0);
	void setGeometryToLeft(int x, int y, int w, int h, int outerw = 0);
	void setGeometryToRight(int x, int y, int w, int h, int outerw = 0);
	void setGeometryToLeft(const QRect &r, int outerw = 0) {
		setGeometryToLeft(r.x(), r.y(), r.width(), r.height(), outerw);
	}

	// Mirrors own-coordinates rects / points in RTL (for painting).
	[[nodiscard]] QRect myrtlrect(int x, int y, int w, int h) const {
		return style::rtlrect(x, y, w, h, width());
	}
	[[nodiscard]] QRect myrtlrect(const QRect &r) const {
		return style::rtlrect(r, width());
	}
	[[nodiscard]] QPoint myrtlpoint(int x, int y) const {
		return style::rtlpoint(x, y, width());
	}

	// Resizes to the width and to the height the widget wants for it.
	void resizeToWidth(int newWidth);
	// The width the widget would like to have, -1 if any.
	[[nodiscard]] virtual int naturalWidth() const {
		return -1;
	}
	// resizeToWidth(min(naturalWidth, newWidth)) for widgets that have one.
	virtual void resizeToNaturalWidth(int newWidth);

	// Tells the widget which part of it is visible inside a scroll area.
	void setVisibleTopBottom(int visibleTop, int visibleBottom);

	void showAnimatedOff() {
		show();
	}

	// update() of a rect given in logical (leading-side) coordinates.
	void rtlupdate(int x, int y, int w, int h) {
		update(myrtlrect(x, y, w, h));
	}
	void rtlupdate(const QRect &r) {
		update(myrtlrect(r));
	}

Q_SIGNALS:
	void sizeChanged(QSize size);
	void widthChanged(int width);
	void heightChanged(int height);
	void geometryChanged(QRect geometry);
	void shownChanged(bool shown);
	// Emitted from the default paintEvent() with the dirty rect.
	void paintRequest(QRect clip);

protected:
	// Returns the height for `newWidth` (and lays children out for it).
	virtual int resizeGetHeight(int newWidth) {
		Q_UNUSED(newWidth);
		return height();
	}
	virtual void visibleTopBottomUpdated(int visibleTop, int visibleBottom) {
		Q_UNUSED(visibleTop);
		Q_UNUSED(visibleBottom);
	}

	bool event(QEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

};

// new Widget(parent, args...) with the parent as the owner.
template <typename Widget, typename Parent, typename ...Args>
inline Widget *CreateChild(Parent *parent, Args &&...args) {
	return new Widget(parent, std::forward<Args>(args)...);
}

} // namespace Ui
