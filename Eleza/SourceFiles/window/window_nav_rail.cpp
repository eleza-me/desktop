#include "window/window_nav_rail.h"

#include "lang_auto.h"
#include "ui/painter.h"
#include "ui/widgets/misc_widgets.h"
#include "styles/style_window.h"

#include <QtGui/QMouseEvent>

namespace Window {

NavRail::NavRail(QWidget *parent) : RpWidget(parent) {
	for (auto i = 0; i != kTabsCount; ++i) {
		_items[i].tab = Tab(i);
	}
	setMouseTracking(true);
	resize(st::navRailWidth, height());
}

NavRail::~NavRail() = default;

QString NavRail::label(Tab tab) const {
	switch (tab) {
	case Tab::Eli: return tr::lng_tab_eli(tr::now);
	case Tab::Chats: return tr::lng_tab_talk(tr::now);
	case Tab::Search: return tr::lng_tab_search(tr::now);
	case Tab::Me: return tr::lng_tab_me(tr::now);
	case Tab::Settings: return tr::lng_settings(tr::now);
	}
	return QString();
}

const style::icon &NavRail::icon(Tab tab, bool active) const {
	switch (tab) {
	case Tab::Eli:
		return active ? st::navRailIconEliActive : st::navRailIconEli;
	case Tab::Chats:
		return active ? st::navRailIconChatsActive : st::navRailIconChats;
	case Tab::Search:
		return active ? st::navRailIconSearchActive : st::navRailIconSearch;
	case Tab::Me:
		return active ? st::navRailIconMeActive : st::navRailIconMe;
	case Tab::Settings:
		return active ? st::navRailIconSettingsActive : st::navRailIconSettings;
	}
	return st::navRailIconEli;
}

void NavRail::setActive(Tab tab) {
	if (_active == tab) {
		return;
	}
	_previousActive = int(_active);
	_active = tab;
	_activeAnimation.start(
		[this] { update(); },
		0.,
		1.,
		st::motionFast,
		anim::easeOutCubic);
	update();
}

void NavRail::setBadge(Tab tab, int count, bool muted) {
	auto &item = _items[int(tab)];
	if (item.badge != count || item.badgeMuted != muted) {
		item.badge = count;
		item.badgeMuted = muted;
		update(item.rect);
	}
}

void NavRail::layoutItems() {
	const auto padding = st::navRailPadding;
	auto top = padding.top();
	for (auto &item : _items) {
		if (item.tab == Tab::Settings) {
			// Pinned to the bottom.
			item.rect = QRect(
				0,
				height() - padding.bottom() - st::navRailItemHeight,
				width(),
				st::navRailItemHeight);
		} else {
			item.rect = QRect(0, top, width(), st::navRailItemHeight);
			top += st::navRailItemHeight + st::navRailItemSkip;
		}
		item.ripple = nullptr;
	}
}

int NavRail::itemAt(QPoint position) const {
	for (auto i = 0; i != kTabsCount; ++i) {
		if (_items[i].rect.contains(position)) {
			return i;
		}
	}
	return -1;
}

void NavRail::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	layoutItems();
}

void NavRail::mouseMoveEvent(QMouseEvent *e) {
	const auto over = itemAt(e->pos());
	if (_over != over) {
		_over = over;
		setCursor((over >= 0) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void NavRail::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_pressed = itemAt(e->pos());
	if (_pressed >= 0) {
		auto &item = _items[_pressed];
		const auto pill = st::navRailPillSize;
		const auto origin = QPoint(
			item.rect.x() + (item.rect.width() - pill.width()) / 2,
			item.rect.y() + st::navRailPillTop);
		if (!item.ripple) {
			item.ripple = std::make_unique<Ui::RippleAnimation>(
				st::navRailRipple,
				Ui::RippleAnimation::RoundRectMask(pill, pill.height() / 2),
				[this] { update(); });
		}
		const auto local = e->pos() - origin;
		item.ripple->add(QRect(QPoint(), pill).contains(local)
			? local
			: QPoint(pill.width() / 2, pill.height() / 2));
	}
}

void NavRail::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (pressed < 0) {
		return;
	}
	if (const auto ripple = _items[pressed].ripple.get()) {
		ripple->lastStop();
	}
	if (itemAt(e->pos()) == pressed) {
		emit tabChosen(_items[pressed].tab);
	}
}

void NavRail::leaveEvent(QEvent *e) {
	if (_over >= 0) {
		_over = -1;
		setCursor(style::cur_default);
		update();
	}
	RpWidget::leaveEvent(e);
}

void NavRail::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), st::navRailBg);
	// The line that separates the rail from the side column.
	p.fillRect(
		myrtlrect(width() - st::lineWidth, 0, st::lineWidth, height()),
		st::columnLineFg);

	const auto progress = _activeAnimation.value(1.);
	const auto pill = st::navRailPillSize;
	for (auto i = 0; i != kTabsCount; ++i) {
		auto &item = _items[i];
		const auto active = (item.tab == _active);
		const auto over = (i == _over);
		const auto pillRect = QRect(
			item.rect.x() + (item.rect.width() - pill.width()) / 2,
			item.rect.y() + st::navRailPillTop,
			pill.width(),
			pill.height());
		const auto activeRatio = active
			? progress
			: (i == _previousActive)
			? (1. - progress)
			: 0.;
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			if (activeRatio > 0.) {
				// The pill grows from the middle when a tab becomes active.
				const auto shrink = (1. - activeRatio) * pill.width() / 4.;
				p.setBrush(style::WithOpacity(st::navRailActiveBg, activeRatio));
				p.drawRoundedRect(
					QRectF(pillRect).marginsRemoved(
						QMarginsF(shrink, 0, shrink, 0)),
					pill.height() / 2.,
					pill.height() / 2.);
			} else if (over) {
				p.setBrush(st::stateHover);
				p.drawRoundedRect(
					pillRect,
					pill.height() / 2.,
					pill.height() / 2.);
			}
		}
		if (item.ripple) {
			item.ripple->paint(p, pillRect.x(), pillRect.y());
			if (item.ripple->empty()) {
				item.ripple = nullptr;
			}
		}
		const auto &glyph = icon(item.tab, active);
		if (!active && over) {
			glyph.paintInCenter(p, pillRect, st::navRailFgOver->c);
		} else {
			glyph.paintInCenter(p, pillRect);
		}

		const auto text = label(item.tab);
		const auto &font = active
			? st::navRailLabelFontActive
			: st::navRailLabelFont;
		p.setFont(font);
		p.setPen(active
			? st::navRailFgActive
			: over
			? st::navRailFgOver
			: st::navRailFg);
		const auto available = item.rect.width() - 2 * st::spaceXs;
		const auto full = font->width(text);
		const auto shown = (full > available)
			? font->elided(text, available)
			: text;
		const auto shownWidth = (full > available) ? font->width(shown) : full;
		p.drawText(
			item.rect.x() + (item.rect.width() - shownWidth) / 2,
			item.rect.y() + st::navRailLabelTop + font->ascent,
			shown);

		if (item.badge > 0) {
			const auto badgeText = Ui::BadgeText(item.badge);
			const auto badgeWidth = Ui::BadgeWidth(badgeText, st::smallBadge);
			// At the top trailing corner of the icon.
			const auto left = style::RightToLeft()
				? (pillRect.x() + st::navRailBadgeShift.x())
				: (pillRect.x() + pillRect.width()
					- st::navRailBadgeShift.x()
					- badgeWidth);
			Ui::PaintBadge(
				p,
				badgeText,
				style::RightToLeft() ? left : (width() - left - badgeWidth),
				pillRect.y() - st::navRailBadgeShift.y(),
				width(),
				st::smallBadge,
				item.badgeMuted);
		} else if (item.badge < 0) {
			const auto x = style::RightToLeft()
				? (pillRect.x() + 14)
				: (pillRect.x() + pillRect.width() - 14);
			Ui::PaintDot(p, QPoint(x, pillRect.y() + 6), 8, st::brandPrimary);
		}
	}
}

} // namespace Window
