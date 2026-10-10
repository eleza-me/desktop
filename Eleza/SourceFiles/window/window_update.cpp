#include "window/window_update.h"

#include "core/core_application.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "ui/painter.h"
#include "ui/widgets/tooltip.h"
#include "updater/updater_checker.h"
#include "styles/style_window.h"
#include "styles/style_window_update.h"

namespace Window {
namespace {

[[nodiscard]] QString OfferedVersion() {
	const auto updater = Core::App().updater();
	return updater ? Lang::Num(updater->info().version) : QString();
}

// Runs `refresh` now and whenever the checker changes its state or the
// language (the wording) changes.
void FollowUpdater(QObject *guard, std::function<void()> refresh) {
	if (const auto updater = Core::App().updater()) {
		QObject::connect(
			updater,
			&Updater::Checker::stateChanged,
			guard,
			refresh);
	}
	refresh();
}

} // namespace

UpdateOffer CurrentUpdateOffer() {
	const auto updater = Core::App().updater();
	if (!updater) {
		return UpdateOffer::None;
	}
	switch (updater->state()) {
	case Updater::State::Ready: return UpdateOffer::Ready;
	case Updater::State::Available: return UpdateOffer::Announced;
	default: return UpdateOffer::None;
	}
}

void ActivateUpdateOffer(std::function<void()> showDetails) {
	switch (CurrentUpdateOffer()) {
	case UpdateOffer::Ready:
		Core::App().restartToUpdate();
		break;
	case UpdateOffer::Announced:
		if (showDetails) {
			showDetails();
		}
		break;
	case UpdateOffer::None: break;
	}
}

// ---- the rail button

UpdateRailButton::UpdateRailButton(
	QWidget *parent,
	std::function<void()> showDetails)
: RippleButton(parent, st::onBrandRipple) {
	resize(st::navRailWidth, st::navRailItemHeight);
	setClickedCallback([showDetails = std::move(showDetails)] {
		ActivateUpdateOffer(showDetails);
	});
	Ui::InstallTooltip(this, [this] {
		return (_offer == UpdateOffer::Ready)
			? tr::lng_update_tooltip(tr::now, lt_version, OfferedVersion())
			: tr::lng_update_available(tr::now, lt_version, OfferedVersion());
	});
	hide();
	FollowUpdater(this, [this] { refresh(); });
}

void UpdateRailButton::refresh() {
	const auto offer = CurrentUpdateOffer();
	if (_offer == offer) {
		return;
	}
	_offer = offer;
	resetRipple();
	update();
	if (_offerChanged) {
		_offerChanged();
	}
}

QRect UpdateRailButton::pillRect() const {
	const auto pill = QSize(st::navRailPillSize);
	return QRect(
		(width() - pill.width()) / 2,
		st::navRailPillTop,
		pill.width(),
		pill.height());
}

QImage UpdateRailButton::prepareRippleMask() const {
	const auto pill = QSize(st::navRailPillSize);
	return Ui::RippleAnimation::RoundRectMask(pill, pill.height() / 2);
}

QPoint UpdateRailButton::rippleOffset() const {
	return pillRect().topLeft();
}

void UpdateRailButton::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto ready = (_offer == UpdateOffer::Ready);
	const auto pill = pillRect();
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(!ready
			? st::updateRailSoftPillBg
			: isOver()
			? st::updateRailPillBgOver
			: st::updateRailPillBg);
		p.drawRoundedRect(pill, pill.height() / 2., pill.height() / 2.);
	}
	paintRipple(p, pill.x(), pill.y());
	(ready ? st::updateRailIcon : st::updateRailSoftIcon).paintInCenter(p, pill);

	const auto text = ready
		? tr::lng_update_rail(tr::now)
		: tr::lng_update_rail_new(tr::now);
	const auto &font = st::updateRailLabelFont;
	const auto available = width() - 2 * st::spaceXs;
	const auto full = font->width(text);
	const auto shown = (full > available) ? font->elided(text, available) : text;
	const auto shownWidth = (full > available) ? font->width(shown) : full;
	p.setFont(font);
	p.setPen(st::updateRailLabelFg);
	p.drawText(
		(width() - shownWidth) / 2,
		st::navRailLabelTop + font->ascent,
		shown);
}

// ---- the banner

UpdateBanner::UpdateBanner(
	QWidget *parent,
	std::function<void()> showDetails)
: RippleButton(parent, st::brandRipple) {
	resize(width(), st::updateBannerHeight);
	setClickedCallback([showDetails = std::move(showDetails)] {
		ActivateUpdateOffer(showDetails);
	});
	hide();
	FollowUpdater(this, [this] { refresh(); });
}

void UpdateBanner::refresh() {
	const auto offer = CurrentUpdateOffer();
	const auto version = OfferedVersion();
	const auto text = (offer == UpdateOffer::Ready)
		? tr::lng_update_banner_ready(tr::now, lt_version, version)
		: (offer == UpdateOffer::Announced)
		? tr::lng_update_banner_available(tr::now, lt_version, version)
		: QString();
	const auto action = (offer == UpdateOffer::Ready)
		? tr::lng_update_button(tr::now)
		: tr::lng_update_details(tr::now);
	const auto changed = (_offer != offer);
	if (!changed && _text == text && _action == action) {
		return;
	}
	_offer = offer;
	_text = text;
	_action = action;
	update();
	if (changed && _offerChanged) {
		_offerChanged();
	}
}

void UpdateBanner::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), st::updateBannerBg);
	paintRipple(p, 0, 0);
	p.fillRect(
		0,
		height() - st::lineWidth,
		width(),
		st::lineWidth,
		st::updateBannerLineFg);

	// [icon] text ………… action, from the leading side.
	const auto padding = int(st::updateBannerPadding);
	const auto &icon = st::updateBannerIcon;
	icon.paint(p, padding, (height() - icon.height()) / 2, width());
	const auto actionWidth = st::updateBannerActionFont->width(_action);
	const auto left = padding + icon.width() + st::updateBannerSkip;
	const auto available = std::max(
		width() - left - st::updateBannerSkip - actionWidth - padding,
		0);
	const auto top = (height() - st::updateBannerFont->height) / 2;
	p.setFont(st::updateBannerFont);
	p.setPen(st::updateBannerFg);
	p.drawTextLeft(
		left,
		top,
		width(),
		st::updateBannerFont->elided(_text, available));
	p.setFont(st::updateBannerActionFont);
	p.setPen(st::updateBannerActionFg);
	p.drawTextRight(padding, top, width(), _action, actionWidth);
}

// ---- the pill

Ui::RoundButton *CreateUpdatePill(
		QWidget *parent,
		std::function<void()> changed) {
	const auto result = new Ui::RoundButton(
		parent,
		tr::lng_update_button(tr::now),
		st::updatePillButton);
	result->setIcon(&st::updatePillIcon);
	result->setClickedCallback([] {
		ActivateUpdateOffer(nullptr);
	});
	Ui::InstallTooltip(result, [] {
		return tr::lng_update_tooltip(tr::now, lt_version, OfferedVersion());
	});
	result->hide();
	FollowUpdater(result, [result, changed = std::move(changed)] {
		const auto shown = (CurrentUpdateOffer() == UpdateOffer::Ready);
		if (result->isHidden() == shown) {
			result->setVisible(shown);
			if (changed) {
				changed();
			}
		}
	});
	return result;
}

} // namespace Window
