#include "ui/widgets/checkbox.h"

#include "ui/painter.h"

#include <algorithm>

namespace Ui {

ToggleView::ToggleView(
	const style::Toggle &st,
	bool checked,
	std::function<void()> update)
: _st(st)
, _checked(checked)
, _update(std::move(update)) {
}

void ToggleView::setChecked(bool checked, bool animated) {
	if (_checked == checked) {
		return;
	}
	_checked = checked;
	if (animated) {
		_animation.start(
			_update,
			checked ? 0. : 1.,
			checked ? 1. : 0.,
			_st.duration,
			anim::easeOutCubic);
	} else {
		_animation.stop();
	}
	if (_update) {
		_update();
	}
}

QSize ToggleView::size() const {
	return QSize(_st.width, _st.height);
}

void ToggleView::paint(QPainter &p, int left, int top, int outerw) {
	const auto progress = _animation.value(_checked ? 1. : 0.);
	const auto track = style::rtlrect(left, top, _st.width, _st.height, outerw);
	auto hq = PainterHighQualityEnabler(p);
	const auto was = p.opacity();
	if (_disabled) {
		p.setOpacity(was * 0.5);
	}
	p.setPen(Qt::NoPen);
	p.setBrush(anim::color(_st.untoggledBg->c, _st.toggledBg->c, progress));
	p.drawRoundedRect(track, _st.height / 2., _st.height / 2.);

	const auto skip = (_st.height - _st.diameter) / 2.;
	const auto travel = _st.width - _st.diameter - 2 * skip;
	// The thumb rests on the leading side when off.
	const auto shift = skip + travel * progress;
	const auto x = style::RightToLeft()
		? (track.x() + track.width() - shift - _st.diameter)
		: (track.x() + shift);
	p.setBrush(_st.thumbFg);
	p.drawEllipse(QRectF(x, track.y() + skip, _st.diameter, _st.diameter));
	p.setOpacity(was);
}

CheckView::CheckView(
	const style::Check &st,
	bool checked,
	std::function<void()> update)
: _st(st)
, _checked(checked)
, _update(std::move(update)) {
}

void CheckView::setChecked(bool checked, bool animated) {
	if (_checked == checked) {
		return;
	}
	_checked = checked;
	if (animated) {
		_animation.start(
			_update,
			checked ? 0. : 1.,
			checked ? 1. : 0.,
			_st.duration,
			anim::easeOutCubic);
	} else {
		_animation.stop();
	}
	if (_update) {
		_update();
	}
}

QSize CheckView::size() const {
	return QSize(_st.diameter, _st.diameter);
}

void CheckView::paint(QPainter &p, int left, int top, int outerw) {
	const auto progress = _animation.value(_checked ? 1. : 0.);
	const auto rect = QRectF(
		style::rtlrect(left, top, _st.diameter, _st.diameter, outerw));
	auto hq = PainterHighQualityEnabler(p);
	const auto half = _st.thickness / 2.;
	p.setPen(QPen(
		anim::color(_st.untoggledFg->c, _st.toggledBg->c, progress),
		_st.thickness));
	p.setBrush(style::WithOpacity(_st.toggledBg, progress));
	p.drawRoundedRect(
		rect.marginsRemoved(QMarginsF(half, half, half, half)),
		_st.radius,
		_st.radius);
	if (progress > 0.) {
		// The tick: three points on a 24px grid (5,12.5) (10,17.5) (19,7).
		const auto scale = _st.diameter / 24.;
		const auto point = [&](double x, double y) {
			return QPointF(rect.x() + x * scale, rect.y() + y * scale);
		};
		auto path = QPainterPath();
		path.moveTo(point(6.5, 12.5));
		path.lineTo(point(10.5, 16.5));
		path.lineTo(point(17.5, 8.5));
		auto pen = QPen(_st.checkFg->c, std::max(1.6, 2. * scale));
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		const auto length = path.length();
		if (progress < 1.) {
			auto partial = QPainterPath();
			partial.moveTo(path.pointAtPercent(0.));
			const auto steps = 12;
			for (auto i = 1; i <= steps; ++i) {
				const auto at = progress * i / steps;
				partial.lineTo(path.pointAtPercent(
					path.percentAtLength(length * at)));
			}
			p.drawPath(partial);
		} else {
			p.drawPath(path);
		}
	}
}

RadioView::RadioView(
	const style::Radio &st,
	bool checked,
	std::function<void()> update)
: _st(st)
, _checked(checked)
, _update(std::move(update)) {
}

void RadioView::setChecked(bool checked, bool animated) {
	if (_checked == checked) {
		return;
	}
	_checked = checked;
	if (animated) {
		_animation.start(
			_update,
			checked ? 0. : 1.,
			checked ? 1. : 0.,
			_st.duration,
			anim::easeOutCubic);
	} else {
		_animation.stop();
	}
	if (_update) {
		_update();
	}
}

QSize RadioView::size() const {
	return QSize(_st.diameter, _st.diameter);
}

void RadioView::paint(QPainter &p, int left, int top, int outerw) {
	const auto progress = _animation.value(_checked ? 1. : 0.);
	const auto rect = QRectF(
		style::rtlrect(left, top, _st.diameter, _st.diameter, outerw));
	auto hq = PainterHighQualityEnabler(p);
	const auto half = _st.thickness / 2.;
	p.setPen(QPen(
		anim::color(_st.untoggledFg->c, _st.toggledFg->c, progress),
		_st.thickness));
	p.setBrush(Qt::NoBrush);
	p.drawEllipse(rect.marginsRemoved(QMarginsF(half, half, half, half)));
	if (progress > 0.) {
		const auto skip = _st.skip + (1. - progress) * (_st.diameter / 2. - _st.skip);
		p.setPen(Qt::NoPen);
		p.setBrush(_st.toggledFg);
		p.drawEllipse(rect.marginsRemoved(QMarginsF(skip, skip, skip, skip)));
	}
}

Checkbox::Checkbox(
	QWidget *parent,
	const QString &text,
	bool checked,
	const style::Checkbox &st)
: RippleButton(parent, st.ripple)
, _st(st)
, _check(std::make_unique<CheckView>(st.check, checked, [this] { update(); }))
, _text(st.style, text) {
	QObject::connect(
		this,
		&AbstractButton::clickedSignal,
		this,
		[this](Qt::KeyboardModifiers, Qt::MouseButton button) {
			if (button == Qt::LeftButton) {
				setChecked(!this->checked());
			}
		});
	resizeToWidth(naturalWidth());
}

Checkbox::~Checkbox() = default;

void Checkbox::setText(const QString &text) {
	_text.setText(_st.style, text);
	resizeToWidth(width());
	update();
}

bool Checkbox::checked() const {
	return _check->checked();
}

void Checkbox::setChecked(bool checked, bool animated) {
	if (_check->checked() != checked) {
		_check->setChecked(checked, animated);
		emit checkedChanged(checked);
	}
}

int Checkbox::naturalWidth() const {
	return _st.margin.left()
		+ _check->size().width()
		+ _st.textSkip
		+ _text.maxWidth()
		+ _st.margin.right();
}

int Checkbox::resizeGetHeight(int newWidth) {
	const auto available = std::max(
		newWidth
			- _st.margin.left()
			- _st.margin.right()
			- _check->size().width()
			- _st.textSkip,
		1);
	const auto textHeight = _text.countHeight(available);
	return _st.margin.top()
		+ std::max(textHeight, _check->size().height())
		+ _st.margin.bottom();
}

QImage Checkbox::prepareRippleMask() const {
	return RippleAnimation::RoundRectMask(size(), st::radiusSm);
}

void Checkbox::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	paintRipple(p, 0, 0);
	if (isDisabled()) {
		p.setOpacity(0.5);
	}
	const auto checkSize = _check->size();
	const auto available = std::max(
		width()
			- _st.margin.left()
			- _st.margin.right()
			- checkSize.width()
			- _st.textSkip,
		1);
	const auto textHeight = _text.countHeight(available);
	const auto lineHeight = _text.minHeight();
	const auto contentHeight = std::max(textHeight, checkSize.height());
	// The check aligns with the first line of the label.
	const auto checkTop = _st.margin.top()
		+ ((textHeight > lineHeight)
			? std::max((lineHeight - checkSize.height()) / 2, 0)
			: (contentHeight - checkSize.height()) / 2);
	_check->paint(p, _st.margin.left(), checkTop, width());
	p.setPen(_st.textFg);
	const auto textLeft = _st.margin.left() + checkSize.width() + _st.textSkip;
	_text.draw(p, {
		.position = QPoint(
			style::RightToLeft() ? (width() - textLeft - available) : textLeft,
			_st.margin.top() + (contentHeight - textHeight) / 2),
		.availableWidth = available,
		.align = style::al_left,
	});
}

Toggle::Toggle(QWidget *parent, bool checked, const style::Toggle &st)
: AbstractButton(parent)
, _view(st, checked, [this] { update(); }) {
	resize(_view.size());
	QObject::connect(
		this,
		&AbstractButton::clickedSignal,
		this,
		[this](Qt::KeyboardModifiers, Qt::MouseButton button) {
			if (button == Qt::LeftButton) {
				setChecked(!this->checked());
			}
		});
}

bool Toggle::checked() const {
	return _view.checked();
}

void Toggle::setChecked(bool checked, bool animated) {
	if (_view.checked() != checked) {
		_view.setChecked(checked, animated);
		emit checkedChanged(checked);
	}
}

void Toggle::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	_view.setDisabled(isDisabled());
	_view.paint(p, 0, 0, width());
}

void RadiobuttonGroup::setValue(int value) {
	if (_hasValue && _value == value) {
		return;
	}
	_hasValue = true;
	_value = value;
	for (const auto button : _buttons) {
		button->handleNewGroupValue(value);
	}
	if (const auto callback = _changedCallback) {
		callback(value);
	}
}

void RadiobuttonGroup::registerButton(Radiobutton *button) {
	if (std::find(_buttons.begin(), _buttons.end(), button) == _buttons.end()) {
		_buttons.push_back(button);
	}
}

void RadiobuttonGroup::unregisterButton(Radiobutton *button) {
	_buttons.erase(
		std::remove(_buttons.begin(), _buttons.end(), button),
		_buttons.end());
}

Radiobutton::Radiobutton(
	QWidget *parent,
	const std::shared_ptr<RadiobuttonGroup> &group,
	int value,
	const QString &text,
	const style::Checkbox &st,
	const style::Radio &radioSt)
: RippleButton(parent, st.ripple)
, _st(st)
, _group(group)
, _value(value)
, _radio(
	radioSt,
	group->hasValue() && group->value() == value,
	[this] { update(); })
, _text(st.style, text) {
	_group->registerButton(this);
	setClickedCallback([this] {
		_group->setValue(_value);
	});
	resizeToWidth(naturalWidth());
}

Radiobutton::~Radiobutton() {
	_group->unregisterButton(this);
}

void Radiobutton::handleNewGroupValue(int value) {
	_radio.setChecked(value == _value, true);
}

int Radiobutton::naturalWidth() const {
	return _st.margin.left()
		+ _radio.size().width()
		+ _st.textSkip
		+ _text.maxWidth()
		+ _st.margin.right();
}

int Radiobutton::resizeGetHeight(int newWidth) {
	const auto available = std::max(
		newWidth
			- _st.margin.left()
			- _st.margin.right()
			- _radio.size().width()
			- _st.textSkip,
		1);
	return _st.margin.top()
		+ std::max(_text.countHeight(available), _radio.size().height())
		+ _st.margin.bottom();
}

QImage Radiobutton::prepareRippleMask() const {
	return RippleAnimation::RoundRectMask(size(), st::radiusSm);
}

void Radiobutton::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	paintRipple(p, 0, 0);
	if (isDisabled()) {
		p.setOpacity(0.5);
	}
	const auto radioSize = _radio.size();
	const auto available = std::max(
		width()
			- _st.margin.left()
			- _st.margin.right()
			- radioSize.width()
			- _st.textSkip,
		1);
	const auto textHeight = _text.countHeight(available);
	const auto lineHeight = _text.minHeight();
	const auto contentHeight = std::max(textHeight, radioSize.height());
	const auto radioTop = _st.margin.top()
		+ ((textHeight > lineHeight)
			? std::max((lineHeight - radioSize.height()) / 2, 0)
			: (contentHeight - radioSize.height()) / 2);
	_radio.paint(p, _st.margin.left(), radioTop, width());
	p.setPen(_st.textFg);
	const auto textLeft = _st.margin.left() + radioSize.width() + _st.textSkip;
	_text.draw(p, {
		.position = QPoint(
			style::RightToLeft() ? (width() - textLeft - available) : textLeft,
			_st.margin.top() + (contentHeight - textHeight) / 2),
		.availableWidth = available,
		.align = style::al_left,
	});
}

} // namespace Ui
