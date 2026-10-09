#include "ui/layers/box_content.h"

#include <QtGui/QKeyEvent>

namespace Ui {

void BoxContent::setDimensionsToContent(int width, RpWidget *content) {
	content->resizeToWidth(width);
	setDimensions(width, content->height());
	QObject::connect(
		content,
		&RpWidget::heightChanged,
		this,
		[this, width](int height) {
			setDimensions(width, height);
		});
}

void BoxContent::setInner(
		object_ptr<RpWidget> inner,
		int topSkip,
		int bottomSkip) {
	if (!_scroll) {
		_scroll = new ScrollArea(this, st::boxScroll);
		_scroll->show();
	}
	_scrollTopSkip = topSkip;
	_scrollBottomSkip = bottomSkip;
	_scroll->setOwnedWidget(std::move(inner));
	updateScrollGeometry();
}

void BoxContent::updateScrollGeometry() {
	if (_scroll) {
		_scroll->setGeometry(
			0,
			_scrollTopSkip,
			width(),
			std::max(height() - _scrollTopSkip - _scrollBottomSkip, 0));
	}
}

void BoxContent::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateScrollGeometry();
}

void BoxContent::keyPressEvent(QKeyEvent *e) {
	if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
		&& _delegate) {
		_delegate->triggerDefaultButton();
	} else {
		RpWidget::keyPressEvent(e);
	}
}

} // namespace Ui
