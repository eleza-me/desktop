#include "calls/calls_panel.h"

#include "calls/calls_service.h"
#include "calls/calls_text.h"
#include "calls/calls_view.h"
#include "calls/calls_widgets.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/media_image.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/tooltip.h"
#include "window/window_main_window.h"
#include "window/window_session_controller.h"
#include "styles/style_calls.h"

#include <QtCore/QDateTime>

namespace Calls {
namespace {

// What the bar shows: a call this device takes part in, or its end.
[[nodiscard]] bool BarWanted(const Service *service) {
	const auto call = service->call();
	if (!call || service->expanded()) {
		return false;
	} else if (call->ended()) {
		return !call->quiet();
	}
	return call->state() != State::Incoming;
}

} // namespace

Bar::Bar(QWidget *parent, Service *service)
: AbstractButton(parent)
, _service(service)
, _userpic(Ui::CreateChild<Support::UserpicButton>(
	this,
	service->session(),
	QString(),
	st::callsBarUserpic))
, _mute(Ui::CreateChild<Button>(this, st::callsBarMute))
, _end(Ui::CreateChild<Button>(this, st::callsBarEnd))
, _close(Ui::CreateChild<Button>(this, st::callsBarClose))
, _again(Ui::CreateChild<Ui::RoundButton>(
	this,
	tr::lng_calls_again(tr::now),
	st::callsBarAgain)) {
	Ui::MakeTransparentForMouse(_userpic);
	_userpic->setBorderColor(st::callsBarBg);
	_again->setIcon(&st::callsBarAgainIcon);
	resize(width(), 0);
	hide();

	setClickedCallback([this] {
		if (const auto call = _service->call(); call && !call->ended()) {
			_service->setExpanded(true);
		}
	});
	_mute->setClickedCallback([this] { _service->toggleMuted(); });
	_end->setClickedCallback([this] { _service->hangup(); });
	_close->setClickedCallback([this] { _service->dismissEnded(); });
	_again->setClickedCallback([this] { _service->callAgain(); });
	Ui::InstallTooltip(_mute, [this] {
		const auto call = _service->call();
		return (call && call->muted())
			? tr::lng_calls_unmute(tr::now)
			: tr::lng_calls_mute(tr::now);
	});
	Ui::InstallTooltip(_end, [] { return tr::lng_calls_end(tr::now); });
	Ui::InstallTooltip(_close, [] { return tr::lng_calls_close(tr::now); });
	Ui::InstallTooltip(this, [this] {
		const auto call = _service->call();
		return (call && !call->ended())
			? tr::lng_calls_expand(tr::now)
			: QString();
	});

	// The duration of the call, once a second.
	_clock.setInterval(1000);
	connect(&_clock, &QTimer::timeout, this, [this] { update(); });
	connect(
		&service->session()->data().users(),
		&Data::Users::changed,
		this,
		[this](const QStringList &ids) {
			if (ids.contains(_peerId)) {
				refresh();
			}
		});
	connect(service, &Service::levelsChanged, this, [this] { update(); });
	refresh();
}

void Bar::refresh() {
	const auto call = _service->call();
	if (call) {
		if (_peerId != call->peerId()) {
			_peerId = call->peerId();
			_userpic->setUserId(_peerId);
		}
		_name = PeerName(_service->session(), _peerId);
		_mute->setActive(call->muted());
		if (call->state() == Calls::State::InCall && !call->ended()) {
			if (!_clock.isActive()) {
				_clock.start();
			}
		} else {
			_clock.stop();
		}
	} else {
		_clock.stop();
	}
	updateControls();
	update();
}

void Bar::setShown(bool shown) {
	if (_shown == shown) {
		return;
	}
	_shown = shown;
	if (shown) {
		show();
	}
	_shownAnimation.start(
		[this] { updateHeight(); },
		shown ? 0. : 1.,
		shown ? 1. : 0.,
		st::motion,
		anim::easeOutCubic);
	updateHeight();
}

void Bar::updateHeight() {
	const auto value = _shownAnimation.value(_shown ? 1. : 0.);
	const auto height = int(std::round(st::callsBarHeight * value));
	if (!_shown && !_shownAnimation.animating()) {
		hide();
		resize(width(), 0);
		return;
	}
	if (this->height() != height) {
		resize(width(), height);
	}
}

void Bar::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateControls();
}

void Bar::updateControls() {
	const auto call = _service->call();
	const auto ended = call && call->ended();
	const auto live = call && !ended;
	_mute->setVisible(live);
	_end->setVisible(live);
	_close->setVisible(ended);
	_again->setVisible(ended && CanCallAgain(*call));
	setPointerCursor(live);

	// Children keep their place relative to the bottom edge, so that the
	// bar slides in as one piece.
	const auto full = int(st::callsBarHeight);
	const auto shift = height() - full;
	const auto &padding = st::callsBarPadding;
	_userpic->moveToLeft(
		padding.left(),
		shift + (full - st::callsBarUserpic) / 2,
		width());
	auto right = padding.right();
	const auto place = [&](QWidget *widget) {
		if (widget->isHidden()) {
			return;
		}
		const auto rp = static_cast<Ui::RpWidget*>(widget);
		rp->moveToRight(right, shift + (full - widget->height()) / 2, width());
		right += widget->width() + st::callsBarButtonSkip;
	};
	if (live) {
		place(_end);
		place(_mute);
	} else {
		place(_close);
		place(_again);
	}
}

void Bar::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), st::callsBarBg);
	const auto call = _service->call();
	if (!call) {
		return;
	}
	const auto full = int(st::callsBarHeight);
	const auto shift = height() - full;
	const auto &padding = st::callsBarPadding;
	const auto left = padding.left()
		+ st::callsBarUserpic
		+ st::callsBarUserpicSkip;
	auto right = padding.right();
	for (const auto widget : std::initializer_list<QWidget*>{
			_mute,
			_end,
			_close,
			_again }) {
		if (!widget->isHidden()) {
			right += widget->width() + st::callsBarButtonSkip;
		}
	}
	const auto available = width() - left - right;
	if (available <= 0) {
		return;
	}
	const auto name = _name.isEmpty()
		? tr::lng_calls_voice_call(tr::now)
		: _name;
	p.setFont(st::callsBarNameFont);
	p.setPen(st::callsBarNameFg);
	p.drawTextLeft(
		left,
		shift + st::callsBarNameTop,
		width(),
		st::callsBarNameFont->elided(name, available));

	auto state = StateText(*call, QDateTime::currentMSecsSinceEpoch());
	if (!call->ended() && call->muted()) {
		state = tr::lng_calls_state_with_muted(
			tr::now,
			lt_state,
			state,
			lt_muted,
			tr::lng_calls_muted(tr::now));
	}
	const auto &font = st::callsBarStateFont;
	const auto elided = font->elided(state, available);
	p.setFont(font);
	p.setPen(StateIsWarning(*call) ? st::callsBarWarnFg : st::callsBarStateFg);
	p.drawTextLeft(left, shift + st::callsBarStateTop, width(), elided);

	// The other side is speaking: a dot after the state line.
	const auto level = (!call->ended() && call->state() == Calls::State::InCall)
		? _service->outputLevel()
		: 0.f;
	if (level > 0.05f) {
		const auto size = int(st::callsBarLevelSize);
		const auto x = left + font->width(elided) + st::callsBarLevelSkip;
		if (x + size <= left + available) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::callsBarStateFg);
			p.setOpacity(0.35 + 0.65 * std::min(level * 1.4f, 1.f));
			p.drawEllipse(myrtlrect(
				x,
				shift + st::callsBarStateTop + (font->height - size) / 2 + 1,
				size,
				size));
		}
	}
}

Host::Host(Service *service, Window::SessionController *controller)
: QObject(controller)
, _service(service)
, _window(controller->window())
, _bar(new Bar(controller->window(), service)) {
	_window->setTopStrip(_bar);
	connect(service, &Service::callChanged, this, [this] { refresh(); });
	connect(service, &Service::callUpdated, this, [this] { refresh(); });
	connect(service, &Service::expandedChanged, this, [this] { refresh(); });
	connect(
		_window->bodyWidget(),
		&Ui::RpWidget::sizeChanged,
		this,
		[this] { updateViewGeometry(); });
	refresh();
}

Host::~Host() {
	if (_view) {
		delete _view.data();
	}
	if (_bar) {
		if (_window) {
			_window->setTopStrip(nullptr);
		}
		delete _bar.data();
	}
}

void Host::refresh() {
	if (_bar) {
		_bar->refresh();
		_bar->setShown(BarWanted(_service));
	}
	const auto expanded = _service->expanded() && _service->call();
	if (expanded && !_view && _window) {
		_view = new View(_window->bodyWidget(), _service);
		// Boxes and toasts stay above the call.
		_view->stackUnder(_window->layers());
		_window->registerDarkOverlay(_view);
		updateViewGeometry();
		_view->show();
		_view->setFocus();
	} else if (!expanded && _view) {
		_view->hide();
		_view->deleteLater();
		_view = nullptr;
	}
	if (_view) {
		_view->refresh();
	}
}

void Host::updateViewGeometry() {
	if (_view && _window) {
		_view->setGeometry(_window->bodyWidget()->rect());
	}
}

} // namespace Calls
