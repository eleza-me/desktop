// Ui::ScrollArea — vertical scrolling with a thin overlay scroll bar.
//
//   const auto scroll = Ui::CreateChild<Ui::ScrollArea>(this);
//   const auto inner = scroll->setOwnedWidget(Ui::object_ptr<Ui::VerticalLayout>(scroll));
//   scroll->setGeometry(...);              // the inner widget follows the width
//   connect(scroll, &Ui::ScrollArea::nearBottom, this, [=] { loadMore(); });
//
//  * The inner widget is resized to the area's width (resizeToWidth for RpWidgets) and
//    reports its own height; RpWidgets get visibleTopBottomUpdated() to paint lazily.
//  * Smooth wheel scrolling, precise scrolling for touchpads, scrollToY(…) animated.
//  * The bar fades in on hover / scroll and hides after a timeout.
//  * Optional hairline shadows at the top / bottom while content is scrolled under them.
#pragma once

#include "ui/rp_widget.h"
#include "ui/effects/animations.h"
#include "styles/style_widgets.h"

#include <QtCore/QTimer>

namespace Ui {

class ScrollBar;
class ScrollShadow;

class ScrollArea : public RpWidget {
	Q_OBJECT

public:
	explicit ScrollArea(
		QWidget *parent,
		const style::ScrollArea &st = st::defaultScrollArea);
	~ScrollArea();

	// Takes ownership. Returns the widget for convenience.
	template <typename Widget>
	Widget *setOwnedWidget(object_ptr<Widget> widget) {
		const auto result = widget.data();
		doSetOwnedWidget(Owned<QWidget>(widget.release()));
		return result;
	}
	[[nodiscard]] QWidget *widget() const {
		return _widget.data();
	}
	template <typename Widget>
	object_ptr<Widget> takeWidget() {
		return Owned(static_cast<Widget*>(doTakeWidget()));
	}

	[[nodiscard]] int scrollTop() const {
		return _scrollTop;
	}
	[[nodiscard]] int scrollTopMax() const;
	[[nodiscard]] int scrollHeight() const; // the content height
	[[nodiscard]] bool isAtTop() const {
		return _scrollTop <= 0;
	}
	[[nodiscard]] bool isAtBottom() const {
		return _scrollTop >= scrollTopMax();
	}

	// Jumps so that `toTop` is at the top; with `toBottom` given, scrolls the
	// least possible to make [toTop, toBottom) visible.
	void scrollToY(int toTop, int toBottom = -1);
	void scrollToYAnimated(int toTop, int duration = 0);
	void scrollToBottom(bool animated = false);
	void scrollToWidget(QWidget *child, bool animated = false);
	void finishScrollAnimation();

	// Distance from an edge (px) at which nearTop / nearBottom fire.
	void setNearEdgeThreshold(int threshold);
	// Wheel events are ignored (a dragged splitter, a locked list).
	void setScrollDisabled(bool disabled);
	// Extra space at the bottom the content can scroll into.
	void setTopBottomShadows(bool top, bool bottom);

	[[nodiscard]] const style::ScrollArea &st() const {
		return _st;
	}

Q_SIGNALS:
	void scrolled();
	void scrollTopChanged(int scrollTop);
	void innerResized();
	// Emitted on every scroll / resize while within the threshold (pagination).
	void nearTop();
	void nearBottom();

protected:
	void resizeEvent(QResizeEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void enterEvent(QEnterEvent *e) override;
	void leaveEvent(QEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	friend class ScrollBar;

	void doSetOwnedWidget(object_ptr<QWidget> widget);
	[[nodiscard]] QWidget *doTakeWidget();
	void applyScrollTop(int top, bool fromAnimation = false);
	void updateInnerGeometry();
	void updateBar();
	void updateShadows();
	void checkEdges();
	void innerHeightChanged();

	const style::ScrollArea &_st;
	object_ptr<QWidget> _widget;
	ScrollBar *_bar = nullptr;
	ScrollShadow *_topShadow = nullptr;
	ScrollShadow *_bottomShadow = nullptr;
	int _scrollTop = 0;
	int _nearThreshold = 0;
	bool _disabled = false;
	bool _inResize = false;
	bool _showTopShadow = true;
	bool _showBottomShadow = true;

	Animations::Simple _scrollAnimation;
	int _animationTarget = 0;

};

} // namespace Ui
