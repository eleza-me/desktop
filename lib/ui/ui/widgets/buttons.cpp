#include "ui/widgets/buttons.h"

#include "ui/effects/radial_animation.h"
#include "ui/widgets/checkbox.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"

#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>

namespace Ui {

AbstractButton::AbstractButton(QWidget *parent) : RpWidget(parent) {
	setMouseTracking(true);
	updateCursor();
}

void AbstractButton::setDisabled(bool disabled) {
	const auto was = _state;
	if (disabled && !(was & StateFlag::Disabled)) {
		_state |= StateFlag::Disabled;
		onStateChanged(was, StateChangeSource::ByUser);
	} else if (!disabled && (was & StateFlag::Disabled)) {
		_state &= ~State(StateFlag::Disabled);
		onStateChanged(was, StateChangeSource::ByUser);
	}
	updateCursor();
	update();
}

void AbstractButton::setPointerCursor(bool enablePointerCursor) {
	if (_enablePointerCursor != enablePointerCursor) {
		_enablePointerCursor = enablePointerCursor;
		updateCursor();
	}
}

void AbstractButton::setAcceptBoth(bool acceptBoth) {
	_acceptBoth = acceptBoth;
}

void AbstractButton::updateCursor() {
	const auto pointer = _enablePointerCursor && !isDisabled();
	setCursor(pointer ? style::cur_pointer : style::cur_default);
}

void AbstractButton::clicked(
		Qt::KeyboardModifiers modifiers,
		Qt::MouseButton button) {
	if (isDisabled()) {
		return;
	}
	_modifiers = modifiers;
	const auto guard = QPointer<AbstractButton>(this);
	if (button == Qt::LeftButton) {
		if (const auto callback = _clickedCallback) {
			callback();
		}
	}
	if (guard) {
		emit clickedSignal(modifiers, button);
	}
}

void AbstractButton::clearState() {
	const auto was = _state;
	_state &= State(StateFlag::Disabled);
	if (was != _state) {
		onStateChanged(was, StateChangeSource::ByUser);
		update();
	}
}

void AbstractButton::setOver(bool over, StateChangeSource source) {
	const auto was = _state;
	if (over && !(was & StateFlag::Over)) {
		_state |= StateFlag::Over;
	} else if (!over && (was & StateFlag::Over)) {
		_state &= ~State(StateFlag::Over);
	} else {
		return;
	}
	onStateChanged(was, source);
	update();
}

void AbstractButton::enterEvent(QEnterEvent *e) {
	setOver(true, StateChangeSource::ByHover);
	RpWidget::enterEvent(e);
}

void AbstractButton::leaveEvent(QEvent *e) {
	if (!isDown()) {
		setOver(false, StateChangeSource::ByHover);
	}
	RpWidget::leaveEvent(e);
}

void AbstractButton::mousePressEvent(QMouseEvent *e) {
	if (isDisabled()) {
		return;
	}
	setOver(rect().contains(e->pos()), StateChangeSource::ByPress);
	if (!isOver()) {
		return;
	}
	if (e->button() == Qt::LeftButton
		|| (_acceptBoth && e->button() == Qt::RightButton)) {
		if (!isDown()) {
			const auto was = _state;
			_state |= StateFlag::Down;
			onStateChanged(was, StateChangeSource::ByPress);
			update();
		}
		e->accept();
	}
}

void AbstractButton::mouseMoveEvent(QMouseEvent *e) {
	if (rect().contains(e->pos())) {
		setOver(true, StateChangeSource::ByHover);
	} else {
		setOver(false, StateChangeSource::ByHover);
	}
}

void AbstractButton::mouseReleaseEvent(QMouseEvent *e) {
	if (!isDown()) {
		return;
	}
	const auto was = _state;
	_state &= ~State(StateFlag::Down);
	const auto guard = QPointer<AbstractButton>(this);
	onStateChanged(was, StateChangeSource::ByPress);
	if (!guard) {
		return;
	}
	update();
	if (was & StateFlag::Over) {
		clicked(e->modifiers(), e->button());
	} else {
		setOver(false, StateChangeSource::ByHover);
	}
}

void AbstractButton::keyPressEvent(QKeyEvent *e) {
	if ((e->key() == Qt::Key_Return
		|| e->key() == Qt::Key_Enter
		|| e->key() == Qt::Key_Space)
		&& focusPolicy() != Qt::NoFocus) {
		clicked(e->modifiers(), Qt::LeftButton);
	} else {
		RpWidget::keyPressEvent(e);
	}
}

RippleButton::RippleButton(QWidget *parent, const style::RippleAnimation &st)
: AbstractButton(parent)
, _st(st) {
}

RippleButton::~RippleButton() = default;

void RippleButton::setForceRippled(bool rippled) {
	if (_forceRippled == rippled) {
		return;
	}
	_forceRippled = rippled;
	if (_forceRippled) {
		ensureRipple();
		if (_ripple->empty()) {
			_ripple->addFading();
		} else {
			_ripple->lastStop();
			_ripple->addFading();
		}
	} else if (_ripple) {
		_ripple->lastStop();
	}
	update();
}

void RippleButton::finishAnimating() {
	if (_ripple && !_forceRippled) {
		_ripple.reset();
		update();
	}
}

void RippleButton::paintRipple(
		QPainter &p,
		int x,
		int y,
		const QColor *colorOverride) {
	if (_ripple) {
		const auto offset = rippleOffset();
		_ripple->paint(p, x + offset.x(), y + offset.y(), colorOverride);
		if (_ripple->empty()) {
			_ripple.reset();
		}
	}
}

void RippleButton::ensureRipple() {
	if (!_ripple) {
		_ripple = std::make_unique<RippleAnimation>(
			_st,
			prepareRippleMask(),
			[this] { update(); });
	}
}

void RippleButton::resetRipple() {
	if (_ripple) {
		_ripple.reset();
		if (_forceRippled) {
			ensureRipple();
			_ripple->addFading();
		}
		update();
	}
}

void RippleButton::onStateChanged(State was, StateChangeSource source) {
	update();
	const auto wasDown = (was & StateFlag::Down) != 0;
	const auto down = isDown();
	if (isDisabled() || wasDown == down) {
		return;
	}
	if (down && source == StateChangeSource::ByPress) {
		ensureRipple();
		_ripple->add(prepareRippleStartPosition());
	} else if (!down && _ripple) {
		if (_forceRippled) {
			return;
		}
		_ripple->lastStop();
	}
}

void RippleButton::resizeEvent(QResizeEvent *e) {
	RpWidget::resizeEvent(e);
	resetRipple();
}

QImage RippleButton::prepareRippleMask() const {
	return RippleAnimation::RectMask(size());
}

QPoint RippleButton::prepareRippleStartPosition() const {
	return mapFromGlobal(QCursor::pos()) - rippleOffset();
}

RoundButton::RoundButton(
	QWidget *parent,
	const QString &text,
	const style::RoundButton &st)
: RippleButton(parent, st.ripple)
, _st(st) {
	setText(text);
}

RoundButton::~RoundButton() = default;

void RoundButton::setText(const QString &text) {
	_text = text;
	_textWidth = _st.font->width(_text);
	refreshSize();
}

void RoundButton::setIcon(const style::icon *icon) {
	_icon = icon;
	refreshSize();
}

void RoundButton::setFullWidth(int newFullWidth) {
	_fullWidth = newFullWidth;
	refreshSize();
}

void RoundButton::refreshSize() {
	resize(_fullWidth > 0 ? _fullWidth : naturalWidth(), _st.height);
	update();
}

int RoundButton::contentWidth() const {
	auto result = _textWidth;
	if (_icon) {
		result += _icon->width() + (_text.isEmpty() ? 0 : _st.iconSkip);
	}
	return result;
}

int RoundButton::naturalWidth() const {
	// A fixed width wins also where the natural one is asked for (rows of a
	// VerticalLayout placed with an alignment).
	return (_fullWidth > 0)
		? _fullWidth
		: std::max(contentWidth() + 2 * _st.padding, _st.minWidth);
}

int RoundButton::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return _st.height;
}

void RoundButton::setLoading(bool loading) {
	if (_loading == loading) {
		return;
	}
	_loading = loading;
	if (_loading) {
		_loadingAnimation = std::make_unique<InfiniteRadialAnimation>(
			[this] { update(); });
		_loadingAnimation->start();
	} else {
		_loadingAnimation = nullptr;
	}
	setAttribute(Qt::WA_TransparentForMouseEvents, _loading);
	clearState();
	update();
}

void RoundButton::setBrandOverride(bool active) {
	setForceRippled(active);
}

void RoundButton::onStateChanged(State was, StateChangeSource source) {
	RippleButton::onStateChanged(was, source);
	const auto wasOver = (was & StateFlag::Over) != 0;
	if (wasOver != isOver()) {
		_overAnimation.start(
			[this] { update(); },
			wasOver ? 1. : 0.,
			isOver() ? 1. : 0.,
			st::motionFast,
			anim::linear);
	}
}

QImage RoundButton::prepareRippleMask() const {
	return RippleAnimation::RoundRectMask(size(), _st.radius);
}

void RoundButton::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto over = _overAnimation.value(
		(isOver() && !isDisabled()) ? 1. : 0.);
	const auto radius = std::min(_st.radius, height() / 2);
	if (isDisabled()) {
		p.setOpacity(0.5);
	}
	{
		auto hq = PainterHighQualityEnabler(p);
		const auto bg = anim::color(_st.textBg->c, _st.textBgOver->c, over);
		if (bg.alpha() > 0) {
			p.setPen(Qt::NoPen);
			p.setBrush(bg);
			p.drawRoundedRect(rect(), radius, radius);
		}
		if (_st.borderFg->c.alpha() > 0) {
			p.setPen(QPen(_st.borderFg->c, 1.));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(
				QRectF(rect()).marginsRemoved(QMarginsF(0.5, 0.5, 0.5, 0.5)),
				radius - 0.5,
				radius - 0.5);
		}
	}
	paintRipple(p, 0, 0);

	const auto fg = anim::color(_st.textFg->c, _st.textFgOver->c, over);
	if (_loading && _loadingAnimation) {
		const auto size = std::min(20, height() - 12);
		_loadingAnimation->draw(
			p,
			QRect((width() - size) / 2, (height() - size) / 2, size, size),
			2.5,
			fg);
		return;
	}
	const auto content = contentWidth();
	const auto available = width() - 2 * std::min(_st.padding, 8);
	auto left = (width() - std::min(content, available)) / 2;
	if (_icon) {
		_icon->paint(p, left, (height() - _icon->height()) / 2, width(), fg);
		left += _icon->width() + _st.iconSkip;
	}
	if (!_text.isEmpty()) {
		p.setFont(_st.font);
		p.setPen(fg);
		const auto textAvailable = width() - left - std::min(_st.padding, 8);
		const auto elided = (_textWidth > textAvailable);
		const auto text = elided
			? _st.font->elided(_text, textAvailable)
			: _text;
		const auto textWidth = elided ? _st.font->width(text) : _textWidth;
		p.drawTextLeft(
			left,
			(height() - _st.font->height) / 2,
			width(),
			text,
			textWidth);
	}
}

FlatButton::FlatButton(QWidget *parent, const QString &text)
: RoundButton(parent, text, st::defaultFlatButton) {
}

FlatButton::FlatButton(
	QWidget *parent,
	const QString &text,
	const style::RoundButton &st)
: RoundButton(parent, text, st) {
}

IconButton::IconButton(QWidget *parent, const style::IconButton &st)
: RippleButton(parent, st.ripple)
, _st(st) {
	resize(_st.width, _st.height);
}

void IconButton::setIconOverride(
		const style::icon *iconOverride,
		const style::icon *iconOverOverride) {
	_iconOverride = iconOverride;
	_iconOverOverride = iconOverOverride;
	update();
}

void IconButton::setIconColorOverride(std::optional<QColor> color) {
	_colorOverride = color;
	update();
}

void IconButton::setDotShown(bool shown) {
	if (_dotShown != shown) {
		_dotShown = shown;
		update();
	}
}

void IconButton::onStateChanged(State was, StateChangeSource source) {
	RippleButton::onStateChanged(was, source);
	const auto wasOver = (was & StateFlag::Over) != 0;
	if (wasOver != isOver()) {
		_overAnimation.start(
			[this] { update(); },
			wasOver ? 1. : 0.,
			isOver() ? 1. : 0.,
			st::motionFast,
			anim::linear);
	}
}

QPoint IconButton::rippleOffset() const {
	return QPoint(
		(width() - _st.rippleSize) / 2,
		(height() - _st.rippleSize) / 2);
}

QImage IconButton::prepareRippleMask() const {
	return RippleAnimation::EllipseMask(
		QSize(_st.rippleSize, _st.rippleSize));
}

QPoint IconButton::prepareRippleStartPosition() const {
	const auto result = mapFromGlobal(QCursor::pos()) - rippleOffset();
	const auto area = QRect(0, 0, _st.rippleSize, _st.rippleSize);
	return area.contains(result)
		? result
		: QPoint(_st.rippleSize / 2, _st.rippleSize / 2);
}

void IconButton::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto over = _overAnimation.value(
		(isOver() && !isDisabled()) ? 1. : 0.);
	if (isDisabled()) {
		p.setOpacity(0.45);
	}
	if (over > 0. && _st.bgOver->c.alpha() > 0) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(style::WithOpacity(_st.bgOver, over));
		const auto offset = rippleOffset();
		p.drawEllipse(offset.x(), offset.y(), _st.rippleSize, _st.rippleSize);
	}
	paintRipple(p, 0, 0);

	const auto useOver = isOver() && !isDisabled();
	const auto &icon = (useOver && _iconOverOverride)
		? *_iconOverOverride
		: _iconOverride
		? *_iconOverride
		: (useOver && !_st.iconOver.empty())
		? _st.iconOver
		: _st.icon;
	if (_colorOverride) {
		icon.paintInCenter(p, rect(), *_colorOverride);
	} else {
		icon.paintInCenter(p, rect());
	}
	if (_dotShown) {
		auto hq = PainterHighQualityEnabler(p);
		const auto size = 8;
		const auto x = (width() + icon.width()) / 2 - size + 2;
		const auto y = (height() - icon.height()) / 2 - 1;
		p.setPen(QPen(st::bgCanvas->c, 2.));
		p.setBrush(st::brandPrimary);
		p.drawEllipse(myrtlrect(x, y, size, size));
	}
}

LinkButton::LinkButton(
	QWidget *parent,
	const QString &text,
	const style::LinkButton &st)
: AbstractButton(parent)
, _st(st) {
	setText(text);
}

void LinkButton::setText(const QString &text) {
	_text = text;
	_textWidth = _st.font->width(_text);
	resize(naturalWidth(), _st.font->height);
	update();
}

void LinkButton::setColorOverride(std::optional<QColor> color) {
	_colorOverride = color;
	update();
}

int LinkButton::naturalWidth() const {
	return _textWidth;
}

int LinkButton::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return _st.font->height;
}

void LinkButton::onStateChanged(State was, StateChangeSource source) {
	Q_UNUSED(was);
	Q_UNUSED(source);
	update();
}

void LinkButton::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto &font = isOver() ? _st.overFont : _st.font;
	p.setFont(font);
	p.setPen(_colorOverride
		? QPen(*_colorOverride)
		: isOver()
		? _st.overColor->p
		: _st.color->p);
	if (isDisabled()) {
		p.setOpacity(0.5);
	}
	if (_textWidth > width()) {
		const auto elided = font->elided(_text, width());
		p.drawTextLeft(0, 0, width(), elided, font->width(elided));
	} else {
		p.drawTextLeft(0, 0, width(), _text, _textWidth);
	}
}

SettingsButton::SettingsButton(
	QWidget *parent,
	const QString &text,
	const style::SettingsButton &st)
: RippleButton(parent, st.ripple)
, _st(st)
, _text(text) {
	resize(width(), _st.height);
}

SettingsButton::~SettingsButton() = default;

void SettingsButton::setText(const QString &text) {
	_text = text;
	update();
}

void SettingsButton::setIcon(const style::icon *icon) {
	_icon = icon;
	update();
}

void SettingsButton::setValueText(const QString &value) {
	_value = value;
	update();
}

void SettingsButton::setChevron(bool shown) {
	_chevron = shown;
	update();
}

void SettingsButton::setDestructive(bool destructive) {
	_destructive = destructive;
	update();
}

void SettingsButton::setToggle(bool checked, bool animated) {
	if (!_toggle) {
		_toggle = std::make_unique<ToggleView>(
			_st.toggle,
			checked,
			[this] { update(); });
		const auto raw = _toggle.get();
		QObject::connect(
			this,
			&AbstractButton::clickedSignal,
			this,
			[this, raw](Qt::KeyboardModifiers, Qt::MouseButton button) {
				if (button == Qt::LeftButton && _toggle.get() == raw) {
					raw->setChecked(!raw->checked(), true);
					emit toggledChanged(raw->checked());
				}
			});
	} else {
		_toggle->setChecked(checked, animated);
	}
	update();
}

bool SettingsButton::toggled() const {
	return _toggle && _toggle->checked();
}

int SettingsButton::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return _st.height;
}

void SettingsButton::onStateChanged(State was, StateChangeSource source) {
	RippleButton::onStateChanged(was, source);
}

void SettingsButton::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto over = isOver() && !isDisabled();
	p.fillRect(rect(), over ? _st.textBgOver : _st.textBg);
	paintRipple(p, 0, 0);
	if (isDisabled()) {
		p.setOpacity(0.5);
	}

	auto left = _st.padding.left();
	auto right = _st.padding.right();
	if (_icon) {
		const auto color = _destructive ? _st.dangerFg->c : _st.iconFg->c;
		_icon->paint(p, left, (height() - _icon->height()) / 2, width(), color);
		left += _st.iconSkip;
	}
	if (_toggle) {
		const auto size = _toggle->size();
		_toggle->paint(
			p,
			width() - right - size.width(),
			(height() - size.height()) / 2,
			width());
		right += size.width() + st::spaceMd;
	} else if (_chevron) {
		_st.chevron.paint(
			p,
			width() - right - _st.chevron.width(),
			(height() - _st.chevron.height()) / 2,
			width());
		right += _st.chevron.width() + st::spaceXs;
	}
	const auto available = width() - left - right;
	if (available <= 0) {
		return;
	}
	auto valueWidth = 0;
	if (!_value.isEmpty()) {
		const auto maxValue = available / 2;
		const auto full = _st.valueFont->width(_value);
		const auto value = (full > maxValue)
			? _st.valueFont->elided(_value, maxValue)
			: _value;
		valueWidth = (full > maxValue) ? _st.valueFont->width(value) : full;
		p.setFont(_st.valueFont);
		p.setPen(_st.valueFg);
		p.drawTextRight(
			right,
			(height() - _st.valueFont->height) / 2,
			width(),
			value,
			valueWidth);
		valueWidth += st::spaceMd;
	}
	const auto textAvailable = available - valueWidth;
	const auto full = _st.font->width(_text);
	const auto text = (full > textAvailable)
		? _st.font->elided(_text, textAvailable)
		: _text;
	p.setFont(_st.font);
	p.setPen(_destructive ? _st.dangerFg : (over ? _st.textFgOver : _st.textFg));
	p.drawTextLeft(
		left,
		(height() - _st.font->height) / 2,
		width(),
		text,
		(full > textAvailable) ? _st.font->width(text) : full);
}

PillButton::PillButton(
	QWidget *parent,
	const QString &text,
	const style::PillButton &st)
: RippleButton(parent, st.ripple)
, _st(st) {
	setText(text);
}

void PillButton::setText(const QString &text) {
	_text = text;
	_textWidth = _st.font->width(_text);
	resize(naturalWidth(), _st.height);
	update();
}

void PillButton::setIcon(const style::icon *icon) {
	_icon = icon;
	resize(naturalWidth(), _st.height);
	update();
}

void PillButton::setActive(bool active, bool animated) {
	if (_active == active) {
		return;
	}
	_active = active;
	if (animated) {
		_activeAnimation.start(
			[this] { update(); },
			active ? 0. : 1.,
			active ? 1. : 0.,
			st::motionFast,
			anim::easeOutCubic);
	} else {
		_activeAnimation.stop();
	}
	update();
}

int PillButton::naturalWidth() const {
	return _textWidth
		+ 2 * _st.padding
		+ (_icon ? (_icon->width() + _st.iconSkip) : 0);
}

int PillButton::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return _st.height;
}

QPainterPath PillButton::shape(const QRectF &rect) const {
	// Like the design's chips: three round corners and a small "tail" corner
	// at the bottom left.
	const auto r = std::min(double(_st.radius), rect.height() / 2.);
	const auto t = std::min(double(_st.tailRadius), r);
	auto path = QPainterPath();
	path.moveTo(rect.left() + r, rect.top());
	path.lineTo(rect.right() - r, rect.top());
	path.arcTo(QRectF(rect.right() - 2 * r, rect.top(), 2 * r, 2 * r), 90, -90);
	path.lineTo(rect.right(), rect.bottom() - r);
	path.arcTo(
		QRectF(rect.right() - 2 * r, rect.bottom() - 2 * r, 2 * r, 2 * r),
		0,
		-90);
	path.lineTo(rect.left() + t, rect.bottom());
	path.arcTo(
		QRectF(rect.left(), rect.bottom() - 2 * t, 2 * t, 2 * t),
		270,
		-90);
	path.lineTo(rect.left(), rect.top() + r);
	path.arcTo(QRectF(rect.left(), rect.top(), 2 * r, 2 * r), 180, -90);
	path.closeSubpath();
	return path;
}

QImage PillButton::prepareRippleMask() const {
	return RippleAnimation::MaskByDrawer(size(), false, [&](QPainter &p) {
		p.drawPath(shape(QRectF(rect())));
	});
}

void PillButton::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto active = _activeAnimation.value(_active ? 1. : 0.);
	{
		auto hq = PainterHighQualityEnabler(p);
		const auto half = 0.5;
		const auto inner = QRectF(rect()).marginsRemoved(
			QMarginsF(half, half, half, half));
		p.setPen(QPen(
			anim::color(_st.borderFg->c, _st.activeBg->c, active),
			1.));
		p.setBrush(anim::color(_st.bg->c, _st.activeBg->c, active));
		p.drawPath(shape(inner));
	}
	paintRipple(p, 0, 0);
	const auto fg = anim::color(_st.textFg->c, _st.activeFg->c, active);
	auto left = _st.padding;
	if (_icon) {
		_icon->paint(p, left, (height() - _icon->height()) / 2, width(), fg);
		left += _icon->width() + _st.iconSkip;
	}
	p.setFont(_st.font);
	p.setPen(fg);
	p.drawTextLeft(
		left,
		(height() - _st.font->height) / 2,
		width(),
		_text,
		_textWidth);
}

} // namespace Ui
