#include "profile/profile_common.h"

namespace Profile {

CenteredWrap::CenteredWrap(QWidget *parent, int maxWidth)
: RpWidget(parent)
, _maxWidth(maxWidth) {
}

void CenteredWrap::setChildWidget(Ui::RpWidget *child) {
	delete _child;
	_child = child;
	if (!child) {
		return;
	}
	child->setParent(this);
	child->show();
	connect(child, &Ui::RpWidget::heightChanged, this, [this] {
		if (!_inResize) {
			resizeToWidth(width());
		}
	});
	if (width() > 0) {
		resizeToWidth(width());
	}
}

void CenteredWrap::setMinHeight(int height) {
	if (_minHeight != height) {
		_minHeight = height;
		if (width() > 0 && !_inResize) {
			resizeToWidth(width());
		}
	}
}

int CenteredWrap::resizeGetHeight(int newWidth) {
	if (!_child) {
		return _minHeight;
	}
	_inResize = true;
	const auto column = CenteredColumn(newWidth, _maxWidth);
	_child->resizeToWidth(column.width);
	_child->move(column.left, 0);
	_inResize = false;
	return std::max(_child->height(), _minHeight);
}

void CenteredWrap::visibleTopBottomUpdated(int visibleTop, int visibleBottom) {
	if (_child) {
		_child->setVisibleTopBottom(visibleTop, visibleBottom);
	}
}

Column CenteredColumn(int outerWidth, int maxWidth) {
	const auto width = std::max(std::min(outerWidth, maxWidth), 0);
	return { (outerWidth - width) / 2, width };
}

} // namespace Profile
