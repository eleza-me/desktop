#include "profile/profile_follow.h"

#include "data/data_graph.h"
#include "data/data_session.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/error_text.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "window/window_session_controller.h"
#include "styles/style_profile.h"

namespace Profile {
namespace {

void SendFollow(
		Window::SessionController *controller,
		QObject *guard,
		const FollowTarget &target,
		const FollowToggle &toggle,
		Fn<void(FollowUi state, bool busy)> update) {
	const auto was = target.state;
	update(toggle.optimistic, true);
	const auto done = [=](const Data::GraphState &state) {
		update(FollowUiOf(state), false);
	};
	const auto fail = [=](const Api::Error &error) {
		update(was, false);
		Support::ShowError(
			controller,
			error,
			tr::lng_profile_action_failed(tr::now));
	};
	auto &graph = controller->session().data().graph();
	if (toggle.follow) {
		graph.follow(target.id, guard, done, fail);
	} else {
		graph.unfollow(target.id, guard, done, fail);
	}
}

} // namespace

void PerformFollowToggle(
		Window::SessionController *controller,
		QObject *guard,
		const FollowTarget &target,
		Fn<void(FollowUi state, bool busy)> update) {
	const auto toggle = ToggleFollow(
		target.state,
		target.isPrivate,
		target.followsYou);
	if (!toggle.confirm) {
		SendFollow(controller, guard, target, toggle, std::move(update));
		return;
	}
	const auto weak = QPointer<QObject>(guard);
	auto args = Ui::ConfirmBoxArgs();
	args.text = tr::lng_profile_unfollow_title(tr::now, lt_name, target.name);
	args.confirmText = tr::lng_unfollow(tr::now);
	args.cancelText = tr::lng_never_mind(tr::now);
	args.destructive = true;
	args.confirmed = [=](std::function<void()> close) {
		close();
		if (weak) {
			SendFollow(controller, weak.data(), target, toggle, update);
		}
	};
	controller->show(Ui::MakeConfirmBox(std::move(args)));
}

QString FollowText(FollowUi state) {
	switch (state) {
	case FollowUi::Following: return tr::lng_following(tr::now);
	case FollowUi::Requested: return tr::lng_follow_requested(tr::now);
	case FollowUi::FollowsYou: return tr::lng_follow_back(tr::now);
	case FollowUi::None: break;
	}
	return tr::lng_follow(tr::now);
}

const style::RoundButton &FollowStyle(FollowUi state, bool small) {
	switch (FollowLookOf(state)) {
	case FollowLook::Outline:
		return small ? st::profileRowFollowing : st::profileFollowing;
	case FollowLook::Soft:
		return small ? st::profileRowRequested : st::profileRequested;
	case FollowLook::Filled: break;
	}
	return small ? st::profileRowFollow : st::profileFollow;
}

const style::icon *FollowIcon(FollowUi state) {
	switch (state) {
	case FollowUi::Following: return &st::profileFollowingIcon;
	case FollowUi::Requested: return &st::profileRequestedIcon;
	case FollowUi::None:
	case FollowUi::FollowsYou: break;
	}
	return &st::profileFollowIcon;
}

int FollowPillWidth(FollowUi state, bool small) {
	const auto &st = FollowStyle(state, small);
	const auto icon = small ? nullptr : FollowIcon(state);
	return st.font->width(FollowText(state))
		+ 2 * st.padding
		+ (icon ? (icon->width() + st.iconSkip) : 0);
}

void PaintPill(
		QPainter &p,
		const QRect &rect,
		const style::RoundButton &st,
		const QString &text,
		bool over,
		bool down,
		const style::icon *icon,
		double opacity) {
	auto hq = PainterHighQualityEnabler(p);
	const auto was = p.opacity();
	p.setOpacity(was * opacity);
	const auto border = (st.borderFg->c.alpha() > 0);
	const auto shape = border
		? QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5)
		: QRectF(rect);
	const auto radius = std::min(double(st.radius), shape.height() / 2.);
	p.setPen(border ? QPen(st.borderFg->c, 1.) : QPen(Qt::NoPen));
	p.setBrush(over ? st.textBgOver : st.textBg);
	p.drawRoundedRect(shape, radius, radius);
	if (down) {
		p.setPen(Qt::NoPen);
		p.setBrush(st.ripple.color);
		p.drawRoundedRect(shape, radius, radius);
	}
	const auto textWidth = st.font->width(text);
	const auto iconWidth = icon ? (icon->width() + st.iconSkip) : 0;
	const auto available = rect.width() - 2 * st.padding - iconWidth;
	const auto shown = (textWidth > available)
		? st.font->elided(text, std::max(available, 0))
		: text;
	const auto shownWidth = std::min(textWidth, std::max(available, 0));
	const auto content = shownWidth + iconWidth;
	// The icon leads the text: on the right in RTL.
	const auto left = rect.x() + (rect.width() - content) / 2;
	const auto color = over ? st.textFgOver->c : st.textFg->c;
	if (icon) {
		const auto iconLeft = style::RightToLeft()
			? (left + content - icon->width())
			: left;
		icon->paintInCenter(
			p,
			QRect(iconLeft, rect.y(), icon->width(), rect.height()),
			color);
	}
	p.setPen(color);
	p.setFont(st.font->f);
	p.drawText(
		style::RightToLeft() ? left : (left + iconWidth),
		rect.y() + (rect.height() - st.font->height) / 2 + st.font->ascent,
		shown);
	p.setOpacity(was);
}

FollowButton::FollowButton(QWidget *parent)
: RippleButton(parent, st::profileFollow.ripple) {
	resize(naturalWidth(), st::profileFollow.height);
}

void FollowButton::setState(FollowUi state) {
	if (_state == state) {
		return;
	}
	_state = state;
	update();
}

void FollowButton::setBusy(bool busy) {
	if (_busy == busy) {
		return;
	}
	_busy = busy;
	setAttribute(Qt::WA_TransparentForMouseEvents, busy);
	if (busy) {
		clearState();
	}
	update();
}

int FollowButton::naturalWidth() const {
	auto result = 0;
	for (const auto state : {
		FollowUi::None,
		FollowUi::Following,
		FollowUi::Requested,
		FollowUi::FollowsYou,
	}) {
		result = std::max(result, FollowPillWidth(state, false));
	}
	return result;
}

int FollowButton::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return st::profileFollow.height;
}

QImage FollowButton::prepareRippleMask() const {
	return Ui::RippleAnimation::RoundRectMask(
		size(),
		std::min(int(st::profileFollow.radius), height() / 2));
}

void FollowButton::onStateChanged(State was, StateChangeSource source) {
	RippleButton::onStateChanged(was, source);
	update();
}

void FollowButton::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto &st = FollowStyle(_state, false);
	PaintPill(
		p,
		rect(),
		st,
		FollowText(_state),
		isOver(),
		false,
		FollowIcon(_state),
		_busy ? 0.6 : 1.);
	paintRipple(p, 0, 0, &st.ripple.color->c);
}

} // namespace Profile
