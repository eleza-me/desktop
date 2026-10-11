#include "calls/calls_incoming_window.h"

#include "calls/calls_text.h"
#include "core/core_application.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/media_image.h"
#include "ui/effects/shadow.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "window/window_main_window.h"
#include "styles/style_calls.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>

#include <cmath>

namespace Calls {
namespace {

constexpr auto kPulsePeriodMs = 1600;

} // namespace

IncomingWindow::IncomingWindow(
	Main::Session *session,
	const QString &callId,
	const QString &peerId)
: RpWidget(nullptr)
, _session(session)
, _callId(callId)
, _peerId(peerId)
, _userpic(new Support::UserpicButton(
	this,
	session,
	peerId,
	st::callsIncomingUserpic))
, _accept(new Ui::RoundButton(
	this,
	tr::lng_calls_accept(tr::now),
	st::callsIncomingAccept))
, _decline(new Ui::RoundButton(
	this,
	tr::lng_calls_decline(tr::now),
	st::callsIncomingDecline))
, _pulse([this](qint64) {
	update();
	return isVisible();
}) {
	// A tool window: above everything, no entry of its own in the taskbar.
	// It can have the focus (the keyboard works once the user gives it).
	setWindowFlags(Qt::Tool
		| Qt::FramelessWindowHint
		| Qt::WindowStaysOnTopHint
		| Qt::NoDropShadowWindowHint);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_ShowWithoutActivating);
	setAttribute(Qt::WA_MacAlwaysShowToolWindow);
	setFocusPolicy(Qt::StrongFocus);
	setLayoutDirection(Ui::LayoutDirection());
	setWindowTitle(tr::lng_calls_state_incoming(tr::now));

	Ui::MakeTransparentForMouse(_userpic);
	_userpic->setBorderColor(st::callsIncomingBg);
	_accept->setIcon(&st::callsIncomingAcceptIcon);
	_decline->setIcon(&st::callsIncomingDeclineIcon);
	// The keys belong to the window, not to whichever button was clicked last.
	_accept->setFocusPolicy(Qt::NoFocus);
	_decline->setFocusPolicy(Qt::NoFocus);
	_accept->setClickedCallback([this] {
		if (_acceptCallback) {
			_acceptCallback();
		}
	});
	_decline->setClickedCallback([this] {
		if (_declineCallback) {
			_declineCallback();
		}
	});
	if (session) {
		connect(
			&session->data().users(),
			&Data::Users::changed,
			this,
			[this](const QStringList &ids) {
				if (ids.contains(_peerId)) {
					refreshName();
				}
			});
	}
	refreshName();
	relayout();
}

IncomingWindow::~IncomingWindow() = default;

void IncomingWindow::setAcceptCallback(std::function<void()> callback) {
	_acceptCallback = std::move(callback);
}

void IncomingWindow::setDeclineCallback(std::function<void()> callback) {
	_declineCallback = std::move(callback);
}

void IncomingWindow::refreshName() {
	const auto name = PeerName(_session, _peerId);
	if (_name != name) {
		_name = name;
		update();
	}
}

QRect IncomingWindow::bodyRect() const {
	return rect().marginsRemoved(st::callsIncomingShadow.extend);
}

void IncomingWindow::relayout() {
	const auto &padding = st::callsIncomingPadding;
	const auto &extend = st::callsIncomingShadow.extend;
	const auto bodyWidth = int(st::callsIncomingWidth);
	const auto buttonsTop = padding.top()
		+ st::callsIncomingUserpic
		+ st::callsIncomingButtonsTop;
	const auto bodyHeight = buttonsTop
		+ _accept->height()
		+ padding.bottom();
	resize(
		bodyWidth + extend.left() + extend.right(),
		bodyHeight + extend.top() + extend.bottom());
	const auto body = bodyRect();
	_userpic->moveToLeft(
		extend.left() + padding.left(),
		body.y() + padding.top(),
		width());
	const auto available = bodyWidth
		- padding.left()
		- padding.right()
		- st::callsIncomingButtonSkip;
	const auto each = available / 2;
	_decline->setFullWidth(each);
	_accept->setFullWidth(available - each);
	// Decline on the leading side, accept on the trailing one.
	_decline->moveToLeft(
		extend.left() + padding.left(),
		body.y() + buttonsTop,
		width());
	_accept->moveToLeft(
		extend.left() + padding.left() + each + st::callsIncomingButtonSkip,
		body.y() + buttonsTop,
		width());
}

void IncomingWindow::showAtCorner() {
	const auto main = Core::App().window();
	const auto margin = int(st::callsIncomingScreenMargin);
	const auto &extend = st::callsIncomingShadow.extend;
	auto anchor = QRect(0, 0, 1280, 720);
	if (Core::App().commandLine().ephemeral() && main) {
		// A screenshot run grabs the main window: stay inside it.
		anchor = QRect(main->mapToGlobal(QPoint()), main->size());
	} else {
		auto screen = main ? main->screen() : nullptr;
		if (!screen) {
			screen = QGuiApplication::primaryScreen();
		}
		if (screen) {
			anchor = screen->availableGeometry();
		}
	}
	const auto body = bodyRect().size();
	// The top trailing corner of the screen (the physical right in both
	// directions: where the system keeps its own notices).
	move(
		anchor.x() + anchor.width() - margin - body.width() - extend.left(),
		anchor.y() + margin - extend.top());
	show();
	raise();
	if (!Ui::AnimationsDisabled()) {
		_pulse.start();
	}
}

void IncomingWindow::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto body = bodyRect();
	const auto radius = int(st::callsIncomingRadius);
	Ui::Shadow::Paint(p, body, radius, st::callsIncomingShadow);
	p.setPen(QPen(st::callsIncomingBorder->c, st::lineWidth));
	p.setBrush(st::callsIncomingBg);
	p.drawRoundedRect(
		QRectF(body).marginsRemoved(QMarginsF(0.5, 0.5, 0.5, 0.5)),
		radius,
		radius);

	// A soft ring that breathes behind the picture while it rings.
	const auto picture = _userpic->geometry();
	const auto phase = _pulse.animating()
		? double((Ui::Now() - _pulse.started()) % kPulsePeriodMs) / kPulsePeriodMs
		: 0.25;
	const auto grow = 0.5 - 0.5 * std::cos(phase * 2. * 3.14159265358979323846);
	const auto spread = st::callsIncomingPulse * (0.35 + 0.65 * grow);
	p.setPen(Qt::NoPen);
	p.setBrush(st::callsIncomingPulseFg);
	p.drawEllipse(QRectF(picture).marginsAdded(
		QMarginsF(spread, spread, spread, spread)));

	const auto &padding = st::callsIncomingPadding;
	const auto &extend = st::callsIncomingShadow.extend;
	const auto textLeft = extend.left()
		+ padding.left()
		+ st::callsIncomingUserpic
		+ st::callsIncomingUserpicSkip
		+ int(st::callsIncomingPulse);
	const auto textWidth = width()
		- textLeft
		- extend.right()
		- padding.right();
	const auto top = body.y() + padding.top();
	p.setFont(st::callsIncomingNameFont);
	p.setPen(st::callsIncomingNameFg);
	const auto name = _name.isEmpty() ? tr::lng_calls_voice_call(tr::now) : _name;
	p.drawTextLeft(
		textLeft,
		top + st::callsIncomingNameTop,
		width(),
		st::callsIncomingNameFont->elided(name, textWidth));
	p.setFont(st::callsIncomingTextFont);
	p.setPen(st::callsIncomingTextFg);
	p.drawTextLeft(
		textLeft,
		top + st::callsIncomingTextTop,
		width(),
		st::callsIncomingTextFont->elided(
			tr::lng_calls_state_incoming(tr::now),
			textWidth));
}

void IncomingWindow::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
		if (!e->isAutoRepeat() && _acceptCallback) {
			_acceptCallback();
		}
	} else if (e->key() == Qt::Key_Escape) {
		if (!e->isAutoRepeat() && _declineCallback) {
			_declineCallback();
		}
	} else {
		RpWidget::keyPressEvent(e);
	}
}

void IncomingWindow::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton && bodyRect().contains(e->pos())) {
		_dragging = true;
		_dragStart = e->globalPosition().toPoint();
		_dragWindowStart = pos();
		// A click is how the window gets the keyboard.
		activateWindow();
		setFocus();
	}
}

void IncomingWindow::mouseMoveEvent(QMouseEvent *e) {
	if (_dragging) {
		move(_dragWindowStart + e->globalPosition().toPoint() - _dragStart);
	}
}

void IncomingWindow::mouseReleaseEvent(QMouseEvent *e) {
	Q_UNUSED(e);
	_dragging = false;
}

} // namespace Calls
