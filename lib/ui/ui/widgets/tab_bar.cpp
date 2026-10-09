#include "ui/widgets/tab_bar.h"

#include "ui/painter.h"

#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>

#include <algorithm>

namespace Ui {

TabBar::TabBar(QWidget *parent, const style::TabBar &st)
: RpWidget(parent)
, _st(st) {
	setMouseTracking(true);
	resize(width(), _st.height);
}

TabBar::~TabBar() = default;

void TabBar::setTabs(const QStringList &tabs) {
	_tabs.clear();
	for (const auto &text : tabs) {
		auto tab = Tab();
		tab.text = text;
		tab.textWidth = _st.font->width(text);
		_tabs.push_back(std::move(tab));
	}
	_active = std::clamp(_active, 0, std::max(count() - 1, 0));
	_indicatorAnimation.stop();
	layoutTabs(width());
	update();
}

void TabBar::setActive(int index, anim::type animated) {
	if (index < 0 || index >= count() || index == _active) {
		return;
	}
	const auto progress = _indicatorAnimation.value(1.);
	const auto was = indicatorTarget(_active);
	_indicatorFrom = anim::interpolateF(_indicatorFrom, was.first, progress);
	_indicatorWidthFrom = anim::interpolateF(
		_indicatorWidthFrom,
		was.second,
		progress);
	_active = index;
	if (animated == anim::type::normal) {
		_indicatorAnimation.start(
			[this] { update(); },
			0.,
			1.,
			_st.duration,
			anim::standard);
	} else {
		_indicatorAnimation.stop();
	}
	update();
}

void TabBar::setStretch(bool stretch) {
	_stretch = stretch;
	layoutTabs(width());
	update();
}

void TabBar::setTabBadge(int index, bool shown) {
	if (index >= 0 && index < count() && _tabs[index].badge != shown) {
		_tabs[index].badge = shown;
		update();
	}
}

int TabBar::naturalWidth() const {
	auto result = 2 * _st.padding;
	for (const auto &tab : _tabs) {
		result += tab.textWidth + 2 * _st.tabPadding;
	}
	result += std::max(count() - 1, 0) * _st.skip;
	return result;
}

void TabBar::layoutTabs(int forWidth) {
	if (_tabs.empty()) {
		return;
	}
	auto left = _st.padding;
	if (_stretch) {
		const auto available = std::max(forWidth - 2 * _st.padding, count());
		const auto each = available / count();
		for (auto &tab : _tabs) {
			tab.left = left;
			tab.width = each;
			tab.ripple = nullptr;
			left += each;
		}
	} else {
		for (auto &tab : _tabs) {
			tab.left = left;
			tab.width = tab.textWidth + 2 * _st.tabPadding;
			tab.ripple = nullptr;
			left += tab.width + _st.skip;
		}
	}
}

std::pair<double, double> TabBar::indicatorTarget(int index) const {
	if (index < 0 || index >= count()) {
		return { 0., 0. };
	}
	const auto &tab = _tabs[index];
	return {
		tab.left + (tab.width - tab.textWidth) / 2. - _st.indicatorExtend,
		tab.textWidth + 2. * _st.indicatorExtend,
	};
}

int TabBar::resizeGetHeight(int newWidth) {
	layoutTabs(newWidth);
	return _st.height;
}

QRect TabBar::tabRect(int index) const {
	const auto &tab = _tabs[index];
	return myrtlrect(tab.left, 0, tab.width, height());
}

int TabBar::tabAt(QPoint position) const {
	for (auto i = 0; i != count(); ++i) {
		if (tabRect(i).contains(position)) {
			return i;
		}
	}
	return -1;
}

void TabBar::mouseMoveEvent(QMouseEvent *e) {
	const auto over = tabAt(e->pos());
	if (_over != over) {
		_over = over;
		setCursor((over >= 0) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void TabBar::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_pressed = tabAt(e->pos());
	if (_pressed >= 0) {
		auto &tab = _tabs[_pressed];
		const auto rect = tabRect(_pressed);
		if (!tab.ripple) {
			tab.ripple = std::make_unique<RippleAnimation>(
				_st.ripple,
				RippleAnimation::RoundRectMask(
					QSize(rect.width(), rect.height() - 2 * _st.rippleSkip),
					_st.rippleRadius),
				[this] { update(); });
		}
		tab.ripple->add(e->pos() - QPoint(rect.x(), _st.rippleSkip));
	}
}

void TabBar::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (pressed >= 0 && pressed < count()) {
		if (const auto ripple = _tabs[pressed].ripple.get()) {
			ripple->lastStop();
		}
		if (tabAt(e->pos()) == pressed && pressed != _active) {
			setActive(pressed);
			emit activeChanged(pressed);
		}
	}
}

void TabBar::leaveEvent(QEvent *e) {
	if (_over >= 0) {
		_over = -1;
		setCursor(style::cur_default);
		update();
	}
	RpWidget::leaveEvent(e);
}

void TabBar::keyPressEvent(QKeyEvent *e) {
	const auto forward = style::RightToLeft() ? Qt::Key_Left : Qt::Key_Right;
	const auto backward = style::RightToLeft() ? Qt::Key_Right : Qt::Key_Left;
	if (e->key() == forward && _active + 1 < count()) {
		setActive(_active + 1);
		emit activeChanged(_active);
	} else if (e->key() == backward && _active > 0) {
		setActive(_active - 1);
		emit activeChanged(_active);
	} else {
		RpWidget::keyPressEvent(e);
	}
}

void TabBar::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	if (_st.bg->c.alpha() > 0) {
		p.fillRect(rect(), _st.bg);
	}
	if (_st.lineFg->c.alpha() > 0) {
		p.fillRect(
			0,
			height() - st::lineWidth,
			width(),
			st::lineWidth,
			_st.lineFg);
	}
	if (_tabs.empty()) {
		return;
	}
	const auto progress = _indicatorAnimation.value(1.);
	for (auto i = 0; i != count(); ++i) {
		auto &tab = _tabs[i];
		const auto rect = tabRect(i);
		if (tab.ripple) {
			tab.ripple->paint(p, rect.x(), _st.rippleSkip);
			if (tab.ripple->empty()) {
				tab.ripple = nullptr;
			}
		}
		const auto active = (i == _active);
		p.setFont(active ? _st.fontActive : _st.font);
		p.setPen(active
			? _st.textFgActive
			: (i == _over)
			? _st.textFgOver
			: _st.textFg);
		const auto &font = active ? _st.fontActive : _st.font;
		const auto textWidth = font->width(tab.text);
		const auto textLeft = rect.x() + (rect.width() - textWidth) / 2;
		const auto textTop = (height() - _st.indicatorHeight - font->height) / 2
			+ _st.textTop;
		p.drawText(textLeft, textTop + font->ascent, tab.text);
		if (tab.badge) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(_st.indicatorFg);
			const auto size = 6;
			const auto x = style::RightToLeft()
				? (textLeft - size - 4)
				: (textLeft + textWidth + 4);
			p.drawEllipse(x, textTop + 2, size, size);
		}
	}
	// The indicator slides from its previous place to the active tab.
	const auto target = indicatorTarget(_active);
	const auto left = (progress < 1.)
		? anim::interpolateF(_indicatorFrom, target.first, progress)
		: target.first;
	const auto indicatorWidth = (progress < 1.)
		? anim::interpolateF(_indicatorWidthFrom, target.second, progress)
		: target.second;
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(_st.indicatorFg);
	const auto x = style::RightToLeft()
		? (width() - left - indicatorWidth)
		: left;
	const auto radius = _st.indicatorHeight / 2.;
	p.drawRoundedRect(
		QRectF(
			x,
			height() - _st.indicatorHeight - _st.indicatorBottom,
			indicatorWidth,
			_st.indicatorHeight),
		radius,
		radius);
}

} // namespace Ui
