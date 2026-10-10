#include "ui/widgets/misc_widgets.h"

#include "ui/widgets/buttons.h"
#include "ui/painter.h"
#include "ui/ui_phrases.h"
#include "ui/ui_utility.h"

#include <QtGui/QLinearGradient>

#include <algorithm>
#include <cmath>

namespace Ui {
namespace {

constexpr auto kShimmerPeriod = 1400;

} // namespace

QString BadgeText(int count, int limit) {
	if (count <= 0) {
		return QString();
	} else if (count > limit) {
		// The bidi algorithm puts the plus on the trailing side by itself.
		return FormatNumber(limit) + QStringLiteral("+");
	}
	return FormatNumber(count);
}

int BadgeWidth(const QString &text, const style::Badge &st) {
	return std::max(st.font->width(text) + 2 * st.padding, st.height);
}

int PaintBadge(
		QPainter &p,
		const QString &text,
		int x,
		int y,
		int outerw,
		const style::Badge &st,
		bool muted) {
	if (text.isEmpty()) {
		return 0;
	}
	const auto width = BadgeWidth(text, st);
	const auto left = style::RightToLeft() ? x : (outerw - x - width);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(muted ? st.bgMuted : st.bg);
	p.drawRoundedRect(
		QRectF(left, y, width, st.height),
		st.height / 2.,
		st.height / 2.);
	p.setFont(st.font);
	p.setPen(muted ? st.fgMuted : st.fg);
	const auto textWidth = st.font->width(text);
	p.drawText(
		left + (width - textWidth) / 2,
		y + (st.height - st.font->height) / 2 + st.font->ascent,
		text);
	return width;
}

void PaintDot(QPainter &p, QPoint center, int diameter, style::color fg) {
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(fg);
	p.drawEllipse(QPointF(center) + QPointF(0.5, 0.5), diameter / 2., diameter / 2.);
}

UnreadBadge::UnreadBadge(QWidget *parent, const style::Badge &st)
: RpWidget(parent)
, _st(st) {
	setAttribute(Qt::WA_TransparentForMouseEvents);
	hide();
}

void UnreadBadge::setCount(int count) {
	setText(BadgeText(count));
}

void UnreadBadge::setText(const QString &text) {
	_text = text;
	if (_text.isEmpty()) {
		hide();
		return;
	}
	resize(BadgeWidth(_text, _st), _st.height);
	show();
	update();
}

void UnreadBadge::setMuted(bool muted) {
	_muted = muted;
	update();
}

void UnreadBadge::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = QPainter(this);
	PaintBadge(p, _text, 0, 0, width(), _st, _muted);
}

Divider::Divider(QWidget *parent, const style::Divider &st)
: RpWidget(parent)
, _st(st) {
	setAttribute(Qt::WA_TransparentForMouseEvents);
	resize(width(), resizeGetHeight(width()));
}

int Divider::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return _st.padding.top() + _st.thickness + _st.padding.bottom();
}

void Divider::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = QPainter(this);
	if (_st.bg->c.alpha() > 0) {
		p.fillRect(rect(), _st.bg);
	}
	p.fillRect(
		myrtlrect(
			_st.padding.left(),
			_st.padding.top(),
			width() - _st.padding.left() - _st.padding.right(),
			_st.thickness),
		_st.fg);
}

EmptyPlaceholder::EmptyPlaceholder(
	QWidget *parent,
	EmptyPlaceholderDescriptor descriptor,
	const style::EmptyPlaceholder &st)
: RpWidget(parent)
, _st(st)
, _icon(descriptor.icon)
, _title(st.titleStyle, descriptor.title)
, _text(st.textStyle, descriptor.text) {
	if (!descriptor.buttonText.isEmpty()) {
		_button = new RoundButton(this, descriptor.buttonText, st.button);
		_button->setClickedCallback(std::move(descriptor.buttonCallback));
	}
}

void EmptyPlaceholder::setTexts(const QString &title, const QString &text) {
	_title.setText(_st.titleStyle, title);
	_text.setText(_st.textStyle, text);
	layout();
	update();
}

int EmptyPlaceholder::contentHeight(int forWidth) const {
	const auto available = std::max(
		std::min(forWidth - 2 * _st.padding, _st.maxWidth),
		1);
	auto result = 0;
	if (_icon) {
		result += _icon->height() + _st.iconSkip;
	}
	if (!_title.isEmpty()) {
		result += _title.countHeight(available);
	}
	if (!_text.isEmpty()) {
		result += _st.textSkip + _text.countHeight(available);
	}
	if (_button) {
		result += _st.buttonSkip + _button->height();
	}
	return result;
}

int EmptyPlaceholder::resizeGetHeight(int newWidth) {
	return contentHeight(newWidth) + 2 * _st.padding;
}

void EmptyPlaceholder::layout() {
	const auto content = contentHeight(width());
	_contentTop = std::max((height() - content) / 2, 0);
	if (_button) {
		_button->resizeToNaturalWidth(width() - 2 * _st.padding);
		_button->move(
			(width() - _button->width()) / 2,
			_contentTop + content - _button->height());
	}
}

void EmptyPlaceholder::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	layout();
}

void EmptyPlaceholder::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto available = std::max(
		std::min(width() - 2 * _st.padding, _st.maxWidth),
		1);
	const auto left = (width() - available) / 2;
	auto top = _contentTop;
	if (_icon) {
		_icon->paintInCenter(p, QRect(0, top, width(), _icon->height()));
		top += _icon->height() + _st.iconSkip;
	}
	if (!_title.isEmpty()) {
		p.setPen(_st.titleFg);
		_title.draw(p, left, top, available, style::al_top);
		top += _title.countHeight(available);
	}
	if (!_text.isEmpty()) {
		top += _st.textSkip;
		p.setPen(_st.textFg);
		_text.draw(p, left, top, available, style::al_top);
	}
}

ServiceLabel::ServiceLabel(QWidget *parent, const QString &text)
: RpWidget(parent) {
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setText(text);
}

void ServiceLabel::setText(const QString &text) {
	_text = text;
	const auto &font = st::serviceLabelFont;
	resize(
		font->width(_text) + 2 * st::serviceLabelPadding.left(),
		font->height
			+ st::serviceLabelPadding.top()
			+ st::serviceLabelPadding.bottom());
	update();
}

void ServiceLabel::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::glassFill);
		p.drawRoundedRect(rect(), height() / 2., height() / 2.);
	}
	p.setFont(st::serviceLabelFont);
	p.setPen(st::textSecondary);
	p.drawTextCentered(rect(), _text);
}

SkeletonAnimation::SkeletonAnimation(std::function<void()> update)
: _update(std::move(update))
, _animation([this](qint64) {
	if (_update) {
		_update();
	}
	return true;
}) {
}

void SkeletonAnimation::start() {
	if (!_animation.animating() && !AnimationsDisabled()) {
		_started = Now();
		_animation.start();
	}
}

void SkeletonAnimation::stop() {
	_animation.stop();
}

void SkeletonAnimation::paint(
		QPainter &p,
		const QRectF &rect,
		double radius,
		const QRect &outer) const {
	const auto elapsed = _animation.animating() ? (Now() - _started) : 0;
	const auto phase = double(elapsed % kShimmerPeriod) / kShimmerPeriod;
	// A soft band of shine sweeping across the whole widget, towards the
	// reading direction.
	const auto band = std::max(outer.width() / 2., 120.);
	const auto travel = outer.width() + 2 * band;
	const auto position = style::RightToLeft()
		? (outer.x() + outer.width() + band - travel * phase)
		: (outer.x() - band + travel * phase);
	auto gradient = QLinearGradient(
		QPointF(position - band, 0),
		QPointF(position + band, 0));
	gradient.setColorAt(0., st::skeletonBg->c);
	gradient.setColorAt(0.5, st::skeletonShine->c);
	gradient.setColorAt(1., st::skeletonBg->c);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(gradient);
	p.drawRoundedRect(rect, radius, radius);
}

SkeletonList::SkeletonList(QWidget *parent, Kind kind, int rows)
: RpWidget(parent)
, _kind(kind)
, _rows(rows)
, _animation([this] { update(); }) {
	setAttribute(Qt::WA_TransparentForMouseEvents);
}

int SkeletonList::rowHeight() const {
	switch (_kind) {
	case Kind::Chats: return st::skeletonChatRowHeight;
	case Kind::Posts: return st::skeletonPostRowHeight;
	case Kind::Lines: return st::skeletonLineRowHeight;
	}
	return st::skeletonChatRowHeight;
}

int SkeletonList::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return _rows ? (_rows * rowHeight()) : height();
}

void SkeletonList::showEvent(QShowEvent *e) {
	_animation.start();
	RpWidget::showEvent(e);
}

void SkeletonList::hideEvent(QHideEvent *e) {
	_animation.stop();
	RpWidget::hideEvent(e);
}

void SkeletonList::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = QPainter(this);
	const auto each = rowHeight();
	const auto count = _rows ? _rows : (height() / each + 1);
	const auto outer = rect();
	const auto block = [&](int x, int y, int w, int h, double radius) {
		_animation.paint(p, QRectF(myrtlrect(x, y, w, h)), radius, outer);
	};
	const auto padding = st::spaceLg;
	for (auto i = 0; i != count; ++i) {
		const auto top = i * each;
		// Vary the line widths a little so it does not look like a table.
		const auto variant = (i * 37) % 5;
		const auto available = width() - 2 * padding;
		if (_kind == Kind::Chats) {
			const auto userpic = st::skeletonUserpicSize;
			block(padding, top + (each - userpic) / 2, userpic, userpic, userpic / 2.);
			const auto left = padding + userpic + st::spaceMd;
			const auto lineWidth = std::max(width() - left - padding, 0);
			block(left, top + each / 2 - 16, lineWidth * (38 + variant * 6) / 100, 12, 6.);
			block(left, top + each / 2 + 6, lineWidth * (72 + variant * 5) / 100, 10, 5.);
		} else if (_kind == Kind::Posts) {
			const auto userpic = st::skeletonUserpicSmall;
			block(padding, top + padding, userpic, userpic, userpic / 2.);
			const auto left = padding + userpic + st::spaceMd;
			const auto lineWidth = std::max(width() - left - padding, 0);
			block(left, top + padding + 2, lineWidth * (30 + variant * 5) / 100, 12, 6.);
			block(left, top + padding + 30, lineWidth, 10, 5.);
			block(left, top + padding + 50, lineWidth * (88 - variant * 4) / 100, 10, 5.);
			block(left, top + padding + 70, lineWidth * (52 + variant * 7) / 100, 10, 5.);
		} else {
			block(padding, top + (each - 10) / 2, available * (60 + variant * 8) / 100, 10, 5.);
		}
	}
}

} // namespace Ui
