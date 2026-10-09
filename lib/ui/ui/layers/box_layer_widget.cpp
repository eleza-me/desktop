#include "ui/layers/box_layer_widget.h"

#include "ui/effects/shadow.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"

#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>

#include <algorithm>

namespace Ui {
namespace {

void PlaceButtons(
		const std::vector<RoundButton*> &buttons,
		RoundButton *leftButton,
		const QRect &area,
		int outerw) {
	// Buttons sit on the trailing side, the default (first added) one last.
	const auto padding = st::boxButtonsPadding;
	auto right = (outerw - area.x() - area.width()) + padding.right();
	const auto top = area.y() + padding.top();
	for (const auto button : buttons) {
		button->resizeToNaturalWidth(area.width());
		button->moveToRight(right, top, outerw);
		right += button->width() + st::boxButtonsSkip;
	}
	if (leftButton) {
		leftButton->resizeToNaturalWidth(area.width());
		leftButton->moveToLeft(area.x() + padding.left(), top, outerw);
	}
}

} // namespace

BoxLayerWidget::BoxLayerWidget(
	LayerStackWidget *stack,
	object_ptr<BoxContent> content)
: LayerWidget(stack)
, _stack(stack)
, _content(std::move(content))
, _contentWidth(st::boxWidth) {
	_content->setParent(this);
	_content->show();
	_content->setDelegate(this);
	updateGeometryInParent();
}

BoxLayerWidget::~BoxLayerWidget() {
	if (_content) {
		emit _content->boxClosing();
	}
}

int BoxLayerWidget::titleHeight() const {
	return _title.isEmpty() ? st::boxTopSkip : st::boxTitleHeight;
}

int BoxLayerWidget::buttonsHeight() const {
	if (_buttons.empty() && !_leftButton) {
		return st::boxBottomSkip;
	}
	const auto padding = st::boxButtonsPadding;
	return padding.top() + st::defaultBoxButton.height + padding.bottom();
}

QRect BoxLayerWidget::bodyRect() const {
	return rect().marginsRemoved(st::boxShadow.extend);
}

void BoxLayerWidget::setTitle(const QString &title) {
	_title.setText(st::boxTitleStyle, title);
	updateGeometryInParent();
	update();
}

RoundButton *BoxLayerWidget::addButton(
		const QString &text,
		std::function<void()> callback,
		const style::RoundButton &st) {
	const auto button = new RoundButton(this, text, st);
	button->setClickedCallback(std::move(callback));
	button->show();
	_buttons.push_back(button);
	updateGeometryInParent();
	return button;
}

RoundButton *BoxLayerWidget::addLeftButton(
		const QString &text,
		std::function<void()> callback,
		const style::RoundButton &st) {
	delete _leftButton;
	_leftButton = new RoundButton(this, text, st);
	_leftButton->setClickedCallback(std::move(callback));
	_leftButton->show();
	updateGeometryInParent();
	return _leftButton;
}

void BoxLayerWidget::clearButtons() {
	for (const auto button : _buttons) {
		delete button;
	}
	_buttons.clear();
	delete _leftButton;
	_leftButton = nullptr;
	updateGeometryInParent();
}

void BoxLayerWidget::setDimensions(int width, int height) {
	if (_contentWidth == width && _contentHeight == height) {
		return;
	}
	_contentWidth = width;
	_contentHeight = height;
	updateGeometryInParent();
}

void BoxLayerWidget::showBox(object_ptr<BoxContent> box, LayerOption option) {
	_stack->showBox(std::move(box), option);
}

void BoxLayerWidget::closeBox() {
	closeLayer();
}

void BoxLayerWidget::triggerDefaultButton() {
	if (!_buttons.empty() && !_buttons.front()->isDisabled()) {
		_buttons.front()->click();
	}
}

void BoxLayerWidget::parentResized() {
	updateGeometryInParent();
}

void BoxLayerWidget::showFinished() {
	_content->showFinished();
}

void BoxLayerWidget::setInnerFocus() {
	_content->setInnerFocus();
}

void BoxLayerWidget::updateGeometryInParent() {
	if (_inGeometryUpdate) {
		return;
	}
	_inGeometryUpdate = true;
	const auto parent = parentWidget();
	const auto &extend = st::boxShadow.extend;
	const auto available = parent
		? parent->size()
		: QSize(st::boxWidth + 2 * st::boxOuterMargin, 600);
	const auto bodyWidth = std::max(
		std::min(_contentWidth, available.width() - 2 * st::boxOuterMargin),
		st::boxMinWidth);
	const auto chrome = titleHeight() + buttonsHeight();
	const auto maxContent = std::max(
		available.height() - 2 * st::boxOuterMargin - chrome,
		st::boxMinContentHeight);
	const auto contentHeight = std::min(_contentHeight, maxContent);
	const auto bodyHeight = chrome + contentHeight;
	const auto body = QRect(
		(available.width() - bodyWidth) / 2,
		std::max((available.height() - bodyHeight) / 2, st::boxOuterMargin / 2),
		bodyWidth,
		bodyHeight);
	setGeometry(body.marginsAdded(extend));
	updateChildren();
	_inGeometryUpdate = false;
	update();
}

void BoxLayerWidget::updateChildren() {
	const auto body = bodyRect();
	_content->setGeometry(
		body.x(),
		body.y() + titleHeight(),
		body.width(),
		std::max(body.height() - titleHeight() - buttonsHeight(), 0));
	PlaceButtons(
		_buttons,
		_leftButton,
		QRect(
			body.x(),
			body.y() + body.height() - buttonsHeight(),
			body.width(),
			buttonsHeight()),
		width());
}

void BoxLayerWidget::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateChildren();
}

void BoxLayerWidget::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto body = bodyRect();
	Shadow::Paint(p, body, st::boxRadius, st::boxShadow);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::boxBg);
		p.drawRoundedRect(body, st::boxRadius, st::boxRadius);
	}
	if (!_title.isEmpty()) {
		p.setPen(st::boxTitleFg);
		const auto padding = st::boxTitlePadding;
		const auto available = body.width() - padding.left() - padding.right();
		_title.drawLeftElided(
			p,
			(body.x() - 0) + padding.left(),
			body.y() + padding.top(),
			available,
			width());
	}
}

void BoxLayerWidget::mousePressEvent(QMouseEvent *e) {
	// The shadow area behaves like the dimmed background.
	if (e->button() == Qt::LeftButton
		&& !bodyRect().contains(e->pos())
		&& _closeByOutsideClick) {
		closeLayer();
	}
}

void BoxLayerWidget::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape) {
		if (_closeByEscape) {
			closeLayer();
		}
	} else if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
		triggerDefaultButton();
	} else {
		e->accept();
	}
}

SheetLayerWidget::SheetLayerWidget(
	LayerStackWidget *stack,
	object_ptr<BoxContent> content)
: LayerWidget(stack)
, _stack(stack)
, _content(std::move(content))
, _close(new IconButton(this, st::sheetCloseButton))
, _wantedWidth(st::sheetWidth) {
	_content->setParent(this);
	_content->show();
	_close->setClickedCallback([this] { closeLayer(); });
	_content->setDelegate(this);
	parentResized();
}

SheetLayerWidget::~SheetLayerWidget() {
	if (_content) {
		emit _content->boxClosing();
	}
}

int SheetLayerWidget::buttonsHeight() const {
	if (_buttons.empty() && !_leftButton) {
		return 0;
	}
	const auto padding = st::boxButtonsPadding;
	return padding.top() + st::defaultBoxButton.height + padding.bottom();
}

void SheetLayerWidget::setTitle(const QString &title) {
	_title.setText(st::boxTitleStyle, title);
	update();
}

RoundButton *SheetLayerWidget::addButton(
		const QString &text,
		std::function<void()> callback,
		const style::RoundButton &st) {
	const auto button = new RoundButton(this, text, st);
	button->setClickedCallback(std::move(callback));
	button->show();
	_buttons.push_back(button);
	updateChildren();
	return button;
}

RoundButton *SheetLayerWidget::addLeftButton(
		const QString &text,
		std::function<void()> callback,
		const style::RoundButton &st) {
	delete _leftButton;
	_leftButton = new RoundButton(this, text, st);
	_leftButton->setClickedCallback(std::move(callback));
	_leftButton->show();
	updateChildren();
	return _leftButton;
}

void SheetLayerWidget::clearButtons() {
	for (const auto button : _buttons) {
		delete button;
	}
	_buttons.clear();
	delete _leftButton;
	_leftButton = nullptr;
	updateChildren();
}

void SheetLayerWidget::setDimensions(int width, int height) {
	// A sheet always takes the full height; only the width is a wish.
	Q_UNUSED(height);
	if (width > 0 && _wantedWidth != width) {
		_wantedWidth = width;
		parentResized();
	}
}

void SheetLayerWidget::showBox(object_ptr<BoxContent> box, LayerOption option) {
	_stack->showBox(std::move(box), option);
}

void SheetLayerWidget::closeBox() {
	closeLayer();
}

void SheetLayerWidget::triggerDefaultButton() {
	if (!_buttons.empty() && !_buttons.front()->isDisabled()) {
		_buttons.front()->click();
	}
}

void SheetLayerWidget::parentResized() {
	const auto parent = parentWidget();
	if (!parent) {
		return;
	}
	const auto sheetWidth = std::min(_wantedWidth, parent->width());
	// The trailing side: right in LTR, left in RTL.
	setGeometry(
		style::RightToLeft() ? 0 : (parent->width() - sheetWidth),
		0,
		sheetWidth,
		parent->height());
	updateChildren();
}

void SheetLayerWidget::showFinished() {
	_content->showFinished();
}

void SheetLayerWidget::setInnerFocus() {
	_content->setInnerFocus();
}

void SheetLayerWidget::updateChildren() {
	const auto top = st::sheetTitleHeight;
	const auto bottom = buttonsHeight();
	_close->moveToRight(
		st::spaceSm,
		(st::sheetTitleHeight - _close->height()) / 2,
		width());
	_content->setGeometry(0, top, width(), std::max(height() - top - bottom, 0));
	PlaceButtons(
		_buttons,
		_leftButton,
		QRect(0, height() - bottom, width(), bottom),
		width());
}

void SheetLayerWidget::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateChildren();
}

void SheetLayerWidget::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), st::boxBg);
	// A hairline on the side that faces the content.
	p.fillRect(
		style::RightToLeft() ? (width() - st::lineWidth) : 0,
		0,
		st::lineWidth,
		height(),
		st::borderSubtle);
	p.fillRect(
		0,
		st::sheetTitleHeight - st::lineWidth,
		width(),
		st::lineWidth,
		st::chromeHairline);
	if (!_title.isEmpty()) {
		p.setPen(st::boxTitleFg);
		const auto left = st::boxTitlePadding.left();
		const auto available = width() - left - _close->width() - 2 * st::spaceSm;
		_title.drawLeftElided(
			p,
			left,
			(st::sheetTitleHeight - _title.minHeight()) / 2,
			available,
			width());
	}
	if (buttonsHeight() > 0) {
		p.fillRect(
			0,
			height() - buttonsHeight(),
			width(),
			st::lineWidth,
			st::chromeHairline);
	}
}

void SheetLayerWidget::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape) {
		if (_closeByEscape) {
			closeLayer();
		}
	} else if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
		triggerDefaultButton();
	} else {
		e->accept();
	}
}

} // namespace Ui
