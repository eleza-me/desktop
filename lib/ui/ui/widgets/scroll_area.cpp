#include "ui/widgets/scroll_area.h"

#include "ui/painter.h"
#include "ui/ui_utility.h"

#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>

#include <algorithm>
#include <cmath>

namespace Ui {
namespace {

constexpr auto kWheelStep = 96; // logical px per wheel notch
constexpr auto kWheelDuration = 180;
constexpr auto kMinThumb = 28;

} // namespace

class ScrollShadow final : public RpWidget {
public:
	ScrollShadow(QWidget *parent, bool top) : RpWidget(parent), _top(top) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
	}

	void setShownRatio(double ratio) {
		ratio = std::clamp(ratio, 0., 1.);
		if (_ratio != ratio) {
			_ratio = ratio;
			setVisible(_ratio > 0.);
			update();
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = QPainter(this);
		p.setOpacity(_ratio);
		const auto line = st::lineWidth;
		p.fillRect(
			0,
			_top ? 0 : (height() - line),
			width(),
			line,
			st::chromeHairline);
	}

private:
	bool _top = false;
	double _ratio = 0.;

};

class ScrollBar final : public RpWidget {
public:
	ScrollBar(ScrollArea *parent, const style::ScrollArea &st)
	: RpWidget(parent)
	, _area(parent)
	, _st(st) {
		setMouseTracking(true);
		_hideTimer.setSingleShot(true);
		QObject::connect(&_hideTimer, &QTimer::timeout, this, [this] {
			if (!_over && !_dragging && !_areaOver) {
				setShown(false);
			}
		});
		hide();
	}

	void refresh() {
		const auto max = _area->scrollTopMax();
		if (max <= 0) {
			_thumb = QRect();
			if (!isHidden()) {
				hide();
			}
			return;
		}
		const auto full = height() - 2 * _st.deltaY;
		const auto content = _area->scrollHeight();
		const auto thumbHeight = std::clamp(
			int(std::lround(double(full) * _area->height() / content)),
			std::min(kMinThumb, full),
			full);
		const auto top = _st.deltaY + int(std::lround(
			double(full - thumbHeight) * _area->scrollTop() / max));
		_thumb = QRect(0, top, width(), thumbHeight);
		if (isHidden()) {
			show();
		}
		update();
	}

	void scrolledNow() {
		if (_area->scrollTopMax() > 0) {
			setShown(true);
			_hideTimer.start(_st.hideTimeout);
		}
	}

	void setAreaOver(bool over) {
		_areaOver = over;
		if (over && _area->scrollTopMax() > 0) {
			setShown(true);
		} else if (!over && !_dragging) {
			_hideTimer.start(_st.hideTimeout);
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		const auto shown = _shownAnimation.value(_shown ? 1. : 0.);
		if (shown <= 0. || _thumb.isEmpty()) {
			return;
		}
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto over = _overAnimation.value((_over || _dragging) ? 1. : 0.);
		p.setOpacity(shown);
		p.setPen(Qt::NoPen);
		// The bar grows from the thin idle width to the full one on hover.
		const auto barWidth = _st.width + (_st.widthOver - _st.width) * over;
		const auto right = _st.deltaX;
		const auto x = style::RightToLeft()
			? double(right)
			: (width() - right - barWidth);
		if (over > 0.) {
			p.setBrush(style::WithOpacity(_st.bgOver, over));
			p.drawRoundedRect(
				QRectF(x, _st.deltaY, barWidth, height() - 2 * _st.deltaY),
				barWidth / 2.,
				barWidth / 2.);
		}
		p.setBrush(anim::color(_st.barBg->c, _st.barBgOver->c, over));
		p.drawRoundedRect(
			QRectF(x, _thumb.y(), barWidth, _thumb.height()),
			barWidth / 2.,
			barWidth / 2.);
	}

	void enterEvent(QEnterEvent *e) override {
		setOver(true);
		RpWidget::enterEvent(e);
	}

	void leaveEvent(QEvent *e) override {
		if (!_dragging) {
			setOver(false);
		}
		RpWidget::leaveEvent(e);
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton || _thumb.isEmpty()) {
			return;
		}
		_area->finishScrollAnimation();
		if (!_thumb.contains(QPoint(_thumb.x(), e->pos().y()))) {
			// A click on the track jumps so the thumb is centered there.
			const auto full = height() - 2 * _st.deltaY - _thumb.height();
			const auto wanted = e->pos().y() - _st.deltaY - _thumb.height() / 2;
			if (full > 0) {
				_area->scrollToY(int(std::lround(
					double(_area->scrollTopMax()) * wanted / full)));
			}
		}
		_dragging = true;
		_dragStartY = e->globalPosition().toPoint().y();
		_dragStartTop = _area->scrollTop();
		setOver(true);
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		if (!_dragging) {
			return;
		}
		const auto full = height() - 2 * _st.deltaY - _thumb.height();
		if (full <= 0) {
			return;
		}
		const auto delta = e->globalPosition().toPoint().y() - _dragStartY;
		_area->scrollToY(_dragStartTop + int(std::lround(
			double(_area->scrollTopMax()) * delta / full)));
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		if (_dragging) {
			_dragging = false;
			if (!rect().contains(e->pos())) {
				setOver(false);
			}
			_hideTimer.start(_st.hideTimeout);
		}
	}

private:
	void setShown(bool shown) {
		if (_shown == shown) {
			return;
		}
		_shown = shown;
		_shownAnimation.start(
			[this] { update(); },
			shown ? 0. : 1.,
			shown ? 1. : 0.,
			_st.duration,
			anim::linear);
		update();
	}

	void setOver(bool over) {
		if (_over == over) {
			return;
		}
		_over = over;
		_overAnimation.start(
			[this] { update(); },
			over ? 0. : 1.,
			over ? 1. : 0.,
			_st.duration,
			anim::linear);
		if (over) {
			setShown(true);
		} else {
			_hideTimer.start(_st.hideTimeout);
		}
		update();
	}

	ScrollArea *_area = nullptr;
	const style::ScrollArea &_st;
	QRect _thumb;
	bool _shown = false;
	bool _over = false;
	bool _areaOver = false;
	bool _dragging = false;
	int _dragStartY = 0;
	int _dragStartTop = 0;
	QTimer _hideTimer;
	Animations::Simple _shownAnimation;
	Animations::Simple _overAnimation;

};

ScrollArea::ScrollArea(QWidget *parent, const style::ScrollArea &st)
: RpWidget(parent)
, _st(st)
, _bar(new ScrollBar(this, st))
, _topShadow(new ScrollShadow(this, true))
, _bottomShadow(new ScrollShadow(this, false)) {
	setFocusPolicy(Qt::NoFocus);
	_topShadow->hide();
	_bottomShadow->hide();
}

ScrollArea::~ScrollArea() = default;

void ScrollArea::doSetOwnedWidget(object_ptr<QWidget> widget) {
	if (_widget) {
		_widget->removeEventFilter(this);
	}
	_widget = std::move(widget);
	_scrollTop = 0;
	if (_widget) {
		_widget->setParent(this);
		_widget->installEventFilter(this);
		_widget->move(0, 0);
		_widget->show();
		_widget->lower();
		updateInnerGeometry();
	}
	_bar->raise();
	_topShadow->raise();
	_bottomShadow->raise();
	updateBar();
	updateShadows();
}

QWidget *ScrollArea::doTakeWidget() {
	if (_widget) {
		_widget->removeEventFilter(this);
	}
	_scrollTop = 0;
	const auto result = _widget.release();
	updateBar();
	return result;
}

int ScrollArea::scrollHeight() const {
	return _widget ? _widget->height() : 0;
}

int ScrollArea::scrollTopMax() const {
	return std::max(scrollHeight() - height(), 0);
}

void ScrollArea::applyScrollTop(int top, bool fromAnimation) {
	if (!fromAnimation) {
		_scrollAnimation.stop();
	}
	top = std::clamp(top, 0, scrollTopMax());
	const auto changed = (_scrollTop != top);
	_scrollTop = top;
	if (_widget) {
		if (_widget->y() != -top) {
			_widget->move(_widget->x(), -top);
		}
		if (const auto rp = qobject_cast<RpWidget*>(_widget.data())) {
			rp->setVisibleTopBottom(top, top + height());
		}
	}
	if (changed) {
		updateBar();
		_bar->scrolledNow();
		updateShadows();
		emit scrollTopChanged(_scrollTop);
		emit scrolled();
		checkEdges();
	}
}

void ScrollArea::scrollToY(int toTop, int toBottom) {
	if (toBottom >= 0) {
		const auto visibleTop = _scrollTop;
		const auto visibleBottom = _scrollTop + height();
		if (toTop >= visibleTop && toBottom <= visibleBottom) {
			return;
		} else if (toTop < visibleTop || (toBottom - toTop) > height()) {
			applyScrollTop(toTop);
		} else {
			applyScrollTop(toBottom - height());
		}
		return;
	}
	applyScrollTop(toTop);
}

void ScrollArea::scrollToYAnimated(int toTop, int duration) {
	toTop = std::clamp(toTop, 0, scrollTopMax());
	if (toTop == _scrollTop) {
		_scrollAnimation.stop();
		return;
	}
	_animationTarget = toTop;
	_scrollAnimation.start([this] {
		applyScrollTop(
			int(std::lround(_scrollAnimation.value(_animationTarget))),
			true);
	}, _scrollTop, toTop, duration ? duration : st::motion, anim::easeOutCubic);
	if (!_scrollAnimation.animating()) {
		applyScrollTop(toTop);
	}
}

void ScrollArea::scrollToBottom(bool animated) {
	if (animated) {
		scrollToYAnimated(scrollTopMax());
	} else {
		scrollToY(scrollTopMax());
	}
}

void ScrollArea::scrollToWidget(QWidget *child, bool animated) {
	if (!_widget || !child || !IsAncestorOf(_widget, child)) {
		return;
	}
	const auto top = child->mapTo(_widget.data(), QPoint()).y();
	if (animated) {
		const auto bottom = top + child->height();
		if (top >= _scrollTop && bottom <= _scrollTop + height()) {
			return;
		}
		scrollToYAnimated((top < _scrollTop) ? top : (bottom - height()));
	} else {
		scrollToY(top, top + child->height());
	}
}

void ScrollArea::finishScrollAnimation() {
	if (_scrollAnimation.animating()) {
		_scrollAnimation.stop();
		applyScrollTop(_animationTarget);
	}
}

void ScrollArea::setNearEdgeThreshold(int threshold) {
	_nearThreshold = threshold;
}

void ScrollArea::setScrollDisabled(bool disabled) {
	_disabled = disabled;
}

void ScrollArea::setTopBottomShadows(bool top, bool bottom) {
	_showTopShadow = top;
	_showBottomShadow = bottom;
	updateShadows();
}

void ScrollArea::updateInnerGeometry() {
	if (!_widget || _inResize) {
		return;
	}
	_inResize = true;
	if (const auto rp = qobject_cast<RpWidget*>(_widget.data())) {
		rp->resizeToWidth(width());
	} else if (_widget->width() != width()) {
		_widget->resize(width(), _widget->height());
	}
	_inResize = false;
	applyScrollTop(_scrollTop, _scrollAnimation.animating());
}

void ScrollArea::updateBar() {
	const auto barWidth = _st.widthOver + 2 * _st.deltaX;
	_bar->setGeometry(
		style::RightToLeft() ? 0 : (width() - barWidth),
		0,
		barWidth,
		height());
	_bar->refresh();
}

void ScrollArea::updateShadows() {
	const auto fade = 12.;
	_topShadow->setGeometry(0, 0, width(), st::lineWidth);
	_bottomShadow->setGeometry(
		0,
		height() - st::lineWidth,
		width(),
		st::lineWidth);
	_topShadow->setShownRatio(_showTopShadow ? (_scrollTop / fade) : 0.);
	_bottomShadow->setShownRatio(_showBottomShadow
		? ((scrollTopMax() - _scrollTop) / fade)
		: 0.);
}

void ScrollArea::checkEdges() {
	const auto threshold = _nearThreshold ? _nearThreshold : height();
	const auto max = scrollTopMax();
	if (_scrollTop <= threshold) {
		emit nearTop();
	}
	if (_scrollTop >= max - threshold) {
		emit nearBottom();
	}
}

void ScrollArea::innerHeightChanged() {
	// Keep the position, just clamp it to the new range.
	applyScrollTop(_scrollTop, _scrollAnimation.animating());
	if (_widget && _widget->y() != -_scrollTop) {
		_widget->move(_widget->x(), -_scrollTop);
	}
	updateBar();
	updateShadows();
	emit innerResized();
	checkEdges();
}

void ScrollArea::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateInnerGeometry();
	updateBar();
	updateShadows();
	checkEdges();
}

void ScrollArea::wheelEvent(QWheelEvent *e) {
	if (_disabled || !_widget) {
		e->ignore();
		return;
	}
	const auto max = scrollTopMax();
	const auto pixel = e->pixelDelta();
	const auto angle = e->angleDelta();
	if (std::abs(angle.x()) > std::abs(angle.y())
		&& std::abs(pixel.x()) >= std::abs(pixel.y())) {
		e->ignore(); // a horizontal gesture, not ours
		return;
	}
	if (!pixel.isNull() && e->phase() != Qt::NoScrollPhase) {
		// Touchpad: follow the fingers exactly.
		const auto wanted = std::clamp(_scrollTop - pixel.y(), 0, max);
		if (wanted == _scrollTop) {
			e->ignore();
			return;
		}
		applyScrollTop(wanted);
		e->accept();
		return;
	}
	const auto delta = int(std::lround(angle.y() / 120. * kWheelStep));
	if (!delta) {
		e->ignore();
		return;
	}
	const auto from = _scrollAnimation.animating() ? _animationTarget : _scrollTop;
	const auto wanted = std::clamp(from - delta, 0, max);
	if (wanted == _scrollTop && !_scrollAnimation.animating()) {
		e->ignore(); // at the edge: let an outer scroll area take it
		return;
	}
	scrollToYAnimated(wanted, kWheelDuration);
	e->accept();
}

void ScrollArea::keyPressEvent(QKeyEvent *e) {
	const auto page = std::max(height() - kWheelStep, kWheelStep);
	switch (e->key()) {
	case Qt::Key_PageUp: scrollToYAnimated(_scrollTop - page); break;
	case Qt::Key_PageDown: scrollToYAnimated(_scrollTop + page); break;
	case Qt::Key_Home:
		if (e->modifiers() & Qt::ControlModifier) {
			scrollToYAnimated(0);
			break;
		}
		[[fallthrough]];
	case Qt::Key_End:
		if (e->key() == Qt::Key_End && (e->modifiers() & Qt::ControlModifier)) {
			scrollToYAnimated(scrollTopMax());
			break;
		}
		[[fallthrough]];
	default: RpWidget::keyPressEvent(e); break;
	}
}

void ScrollArea::enterEvent(QEnterEvent *e) {
	_bar->setAreaOver(true);
	RpWidget::enterEvent(e);
}

void ScrollArea::leaveEvent(QEvent *e) {
	_bar->setAreaOver(false);
	RpWidget::leaveEvent(e);
}

bool ScrollArea::eventFilter(QObject *object, QEvent *event) {
	if (object == _widget.data() && event->type() == QEvent::Resize) {
		const auto resize = static_cast<QResizeEvent*>(event);
		if (resize->oldSize().height() != resize->size().height()) {
			// Let the widget handle the event first.
			PostponeCall(this, [this] { innerHeightChanged(); });
			if (_widget->height() - height() < _scrollTop) {
				// Shrunk below the current position: clamp right away to
				// avoid a frame of empty space.
				innerHeightChanged();
			}
		}
	}
	return RpWidget::eventFilter(object, event);
}

} // namespace Ui
