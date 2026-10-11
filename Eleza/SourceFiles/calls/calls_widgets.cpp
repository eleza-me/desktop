#include "calls/calls_widgets.h"

#include "ui/painter.h"

namespace Calls {

Button::Button(QWidget *parent, const style::CallButton &st)
: RippleButton(parent, st.ripple)
, _st(st) {
	resize(_st.size, _st.size);
}

void Button::setActive(bool active) {
	if (_active != active) {
		_active = active;
		update();
	}
}

void Button::onStateChanged(State was, StateChangeSource source) {
	RippleButton::onStateChanged(was, source);
	if (((was & StateFlag::Over) != 0) != isOver()) {
		update();
	}
}

QImage Button::prepareRippleMask() const {
	return Ui::RippleAnimation::EllipseMask(size());
}

void Button::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(_active
			? _st.bgActive
			: (isOver() && !isDisabled())
			? _st.bgOver
			: _st.bg);
		p.drawEllipse(rect());
	}
	paintRipple(p, 0, 0);
	if (isDisabled()) {
		p.setOpacity(0.5);
	}
	(_active ? _st.iconActive : _st.icon).paintInCenter(p, rect());
}

LabeledButton::LabeledButton(
	QWidget *parent,
	const style::CallButton &st,
	const QString &label)
: RpWidget(parent)
, _button(Ui::CreateChild<Button>(this, st)) {
	setLabel(label);
}

void LabeledButton::setLabel(const QString &label) {
	_label = label;
	const auto &font = st::callsViewLabelFont;
	const auto width = std::max(
		_button->width(),
		font->width(label) + 2 * int(st::spaceXs));
	resize(
		width,
		_button->height() + st::callsViewLabelTop + font->height);
	_button->move((width - _button->width()) / 2, 0);
	update();
}

void LabeledButton::setClickedCallback(std::function<void()> callback) {
	_button->setClickedCallback(std::move(callback));
}

void LabeledButton::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.setFont(st::callsViewLabelFont);
	p.setPen(st::callsViewLabelFg);
	p.setOpacity(0.86);
	p.drawTextCentered(
		QRect(
			0,
			_button->height() + st::callsViewLabelTop,
			width(),
			st::callsViewLabelFont->height),
		_label);
}

} // namespace Calls
