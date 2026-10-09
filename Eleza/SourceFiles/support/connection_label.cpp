#include "support/connection_label.h"

#include "api/api_client.h"
#include "data/data_session.h"
#include "data/data_sync.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "ui/painter.h"
#include "ui/widgets/top_bar.h"
#include "styles/style_support.h"

namespace Support {
namespace {

// How long «در حال اتصال…» may stay while HTTP requests already work.
constexpr auto kFallbackGrace = 8000;

} // namespace

QString ConnectionText(ConnectionStatus status) {
	switch (status) {
	case ConnectionStatus::Connecting:
		return tr::lng_support_connecting(tr::now);
	case ConnectionStatus::Updating:
		return tr::lng_updating(tr::now);
	case ConnectionStatus::WaitingForNetwork:
		return tr::lng_support_waiting_for_network(tr::now);
	case ConnectionStatus::Online:
		break;
	}
	return QString();
}

ConnectionStatus ComputeConnectionStatus(
		Rt::State state,
		bool updating,
		bool httpReachable,
		bool fallbackWorks) {
	if (!httpReachable) {
		return ConnectionStatus::WaitingForNetwork;
	}
	const auto settled = updating
		? ConnectionStatus::Updating
		: ConnectionStatus::Online;
	switch (state) {
	case Rt::State::Connected:
	case Rt::State::Stopped: // realtime is switched off: HTTP only
		return settled;
	case Rt::State::Updating:
		return ConnectionStatus::Updating;
	case Rt::State::Connecting:
	case Rt::State::WaitingForNetwork:
		return fallbackWorks ? settled : ConnectionStatus::Connecting;
	}
	return settled;
}

ConnectionState *ConnectionState::For(Main::Session *session) {
	if (!session) {
		return nullptr;
	}
	const auto existing = session->findChild<ConnectionState*>(
		QString(),
		Qt::FindDirectChildrenOnly);
	return existing ? existing : new ConnectionState(session);
}

ConnectionState::ConnectionState(Main::Session *session)
: QObject(session)
, _session(session) {
	_fallbackTimer.setSingleShot(true);
	QObject::connect(&_fallbackTimer, &QTimer::timeout, this, [this] {
		if (_session->api().reachable()
			&& _session->connectionState() != Rt::State::Connected) {
			_fallbackWorks = true;
			refresh();
		}
	});
	QObject::connect(
		session,
		&Main::Session::connectionStateChanged,
		this,
		[this](Rt::State state) {
			if (state == Rt::State::Connected || state == Rt::State::Updating) {
				// The next loss of the socket shows «connecting» again.
				_fallbackWorks = false;
				_fallbackTimer.stop();
			} else if (!_fallbackWorks && !_fallbackTimer.isActive()) {
				_fallbackTimer.start(kFallbackGrace);
			}
			refresh();
		});
	QObject::connect(
		&session->data().sync(),
		&Data::Sync::updatingChanged,
		this,
		[this](bool updating) {
			// A difference read over HTTP went through: the fallback works.
			if (!updating
				&& _session->api().reachable()
				&& _session->connectionState() != Rt::State::Connected
				&& _session->data().sync().pts().has_value()) {
				_fallbackWorks = true;
			}
			refresh();
		});
	QObject::connect(
		&session->api(),
		&Api::Client::reachableChanged,
		this,
		[this](bool reachable) {
			if (!reachable) {
				_fallbackWorks = false;
			} else if (!_fallbackTimer.isActive()) {
				_fallbackTimer.start(kFallbackGrace);
			}
			refresh();
		});
	if (session->connectionState() != Rt::State::Connected) {
		_fallbackTimer.start(kFallbackGrace);
	}
	_status = ComputeConnectionStatus(
		session->connectionState(),
		session->data().sync().updating(),
		session->api().reachable(),
		_fallbackWorks);
}

void ConnectionState::refresh() {
	const auto status = ComputeConnectionStatus(
		_session->connectionState(),
		_session->data().sync().updating(),
		_session->api().reachable(),
		_fallbackWorks);
	if (_status != status) {
		_status = status;
		emit changed();
	}
}

ConnectionTitle::ConnectionTitle(
	Ui::TopBar *bar,
	Main::Session *session,
	const QString &title,
	Mode mode)
: QObject(bar)
, _bar(bar)
, _state(ConnectionState::For(session))
, _mode(mode)
, _title(title) {
	if (_state) {
		QObject::connect(
			_state.data(),
			&ConnectionState::changed,
			this,
			[this] { apply(); });
	}
	apply();
}

void ConnectionTitle::setTitle(const QString &title) {
	_title = title;
	apply();
}

void ConnectionTitle::setSubtitle(const QString &subtitle, bool accent) {
	_subtitle = subtitle;
	_subtitleAccent = accent;
	apply();
}

void ConnectionTitle::apply() {
	if (!_bar) {
		return;
	}
	const auto text = _state ? _state->text() : QString();
	if (_mode == Mode::Title) {
		_bar->setTitle(text.isEmpty() ? _title : text);
		_bar->setSubtitle(_subtitle, _subtitleAccent);
	} else {
		_bar->setTitle(_title);
		if (text.isEmpty()) {
			_bar->setSubtitle(_subtitle, _subtitleAccent);
		} else {
			_bar->setSubtitle(text, false);
		}
	}
}

ConnectionLabel::ConnectionLabel(QWidget *parent, Main::Session *session)
: RpWidget(parent)
, _state(ConnectionState::For(session))
, _spinner([this] { update(); }) {
	if (_state) {
		QObject::connect(
			_state.data(),
			&ConnectionState::changed,
			this,
			[this] { refresh(); });
	}
	refresh();
}

void ConnectionLabel::refresh() {
	_text = _state ? _state->text() : QString();
	if (_text.isEmpty()) {
		_spinner.stop();
		hide();
	} else {
		resize(naturalWidth(), st::connectionLabelHeight);
		show();
		_spinner.start();
		update();
	}
}

int ConnectionLabel::naturalWidth() const {
	return st::connectionLabelSpinner
		+ st::connectionLabelSkip
		+ st::connectionLabelFont->width(_text);
}

int ConnectionLabel::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return st::connectionLabelHeight;
}

void ConnectionLabel::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	if (_text.isEmpty()) {
		return;
	}
	auto p = Painter(this);
	const auto size = int(st::connectionLabelSpinner);
	const auto spinner = myrtlrect(0, (height() - size) / 2, size, size);
	_spinner.draw(p, spinner, 2., st::textTertiary->c);
	p.setFont(st::connectionLabelFont);
	p.setPen(st::textSecondary);
	p.drawTextLeft(
		size + st::connectionLabelSkip,
		(height() - st::connectionLabelFont->height) / 2,
		width(),
		_text);
}

} // namespace Support
