#include "ui/widgets/top_bar.h"

#include "ui/widgets/buttons.h"
#include "ui/painter.h"

#include <QtGui/QMouseEvent>

#include <algorithm>

namespace Ui {

TopBar::TopBar(QWidget *parent, const style::TopBar &st)
: RpWidget(parent)
, _st(st)
, _back(new IconButton(this, st.back)) {
	_back->hide();
	_back->setClickedCallback([this] { emit backRequested(); });
	setMouseTracking(true);
	resize(width(), _st.height);
}

void TopBar::setTitle(const QString &title) {
	_title.setText(_st.titleStyle, title);
	update();
}

void TopBar::setSubtitle(const QString &subtitle, bool accent) {
	_subtitle.setText(_st.subtitleStyle, subtitle);
	_subtitleAccent = accent;
	update();
}

void TopBar::setBackShown(bool shown) {
	if (_backShown != shown) {
		_backShown = shown;
		_back->setVisible(shown);
		updateControls();
	}
}

void TopBar::setTitleClickable(bool clickable) {
	_titleClickable = clickable;
}

void TopBar::setCentered(bool centered) {
	_centered = centered;
	update();
}

IconButton *TopBar::addAction(const style::IconButton &st) {
	const auto button = new IconButton(this, st);
	button->show();
	_actions.push_back(button);
	updateControls();
	return button;
}

void TopBar::clearActions() {
	for (const auto button : _actions) {
		delete button;
	}
	_actions.clear();
	updateControls();
}

void TopBar::doSetLeadingWidget(object_ptr<RpWidget> widget) {
	_leading = std::move(widget);
	if (_leading) {
		_leading->setParent(this);
		_leading->show();
	}
	updateControls();
}

int TopBar::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return _st.height;
}

void TopBar::updateControls() {
	auto left = _st.padding.left();
	if (_backShown) {
		_back->moveToLeft(left, (height() - _back->height()) / 2, width());
		left += _back->width() + _st.backSkip;
	} else {
		left += _st.titleLeftNoBack;
	}
	if (_leading) {
		_leading->moveToLeft(left, (height() - _leading->height()) / 2, width());
		left += _leading->width() + _st.leadingSkip;
	}
	auto right = _st.padding.right();
	for (auto i = _actions.rbegin(); i != _actions.rend(); ++i) {
		const auto button = *i;
		button->moveToRight(right, (height() - button->height()) / 2, width());
		right += button->width();
	}
	if (!_actions.empty()) {
		right += _st.actionsSkip;
	}
	_titleLeft = left;
	_titleRight = right;
	update();
}

QRect TopBar::titleRect() const {
	return QRect(
		_titleLeft,
		0,
		std::max(width() - _titleLeft - _titleRight, 0),
		height());
}

void TopBar::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateControls();
}

void TopBar::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), _st.bg);
	p.fillRect(
		0,
		height() - st::lineWidth,
		width(),
		st::lineWidth,
		_st.lineFg);

	const auto area = titleRect();
	if (area.width() <= 0) {
		return;
	}
	const auto hasSubtitle = !_subtitle.isEmpty();
	const auto titleHeight = _title.minHeight();
	const auto subtitleHeight = hasSubtitle ? _subtitle.minHeight() : 0;
	const auto total = titleHeight + subtitleHeight;
	auto top = (height() - total) / 2;
	auto x = area.x();
	auto available = area.width();
	auto align = style::al_left;
	if (_centered) {
		// Center in the bar when the side controls leave room for it.
		const auto side = std::max(_titleLeft, _titleRight);
		x = side;
		available = std::max(width() - 2 * side, 0);
		align = style::al_top;
	}
	p.setPen(_st.titleFg);
	_title.drawLeftElided(p, x, top, available, width(), 1, align);
	if (hasSubtitle) {
		top += titleHeight;
		p.setPen(_subtitleAccent ? _st.subtitleFgAccent : _st.subtitleFg);
		_subtitle.drawLeftElided(p, x, top, available, width(), 1, align);
	}
}

void TopBar::mousePressEvent(QMouseEvent *e) {
	if (_titleClickable
		&& e->button() == Qt::LeftButton
		&& myrtlrect(titleRect()).contains(e->pos())) {
		_titlePressed = true;
	}
}

void TopBar::mouseReleaseEvent(QMouseEvent *e) {
	if (std::exchange(_titlePressed, false)
		&& myrtlrect(titleRect()).contains(e->pos())) {
		emit titleClicked();
	}
}

void TopBar::mouseMoveEvent(QMouseEvent *e) {
	setCursor((_titleClickable && myrtlrect(titleRect()).contains(e->pos()))
		? style::cur_pointer
		: style::cur_default);
}

void TopBar::leaveEvent(QEvent *e) {
	setCursor(style::cur_default);
	RpWidget::leaveEvent(e);
}

} // namespace Ui
