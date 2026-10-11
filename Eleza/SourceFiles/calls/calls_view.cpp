#include "calls/calls_view.h"

#include "calls/calls_service.h"
#include "calls/calls_text.h"
#include "calls/calls_widgets.h"
#include "core/core_application.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/media_image.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/tooltip.h"
#include "styles/style_calls.h"

#include <QtCore/QDateTime>
#include <QtGui/QKeyEvent>
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QMediaDevices>

#include <cmath>

namespace Calls {

void FillDevicesMenu(Ui::PopupMenu *menu, Service *service) {
	const auto &settings = Core::App().settings().calls();
	const auto weak = QPointer<Service>(service);
	const auto add = [&](
			const QString &title,
			const QList<QAudioDevice> &devices,
			const QByteArray &chosen,
			bool input) {
		// The heading of the group.
		auto heading = Ui::MenuItem();
		heading.text = title;
		heading.disabled = true;
		menu->addItem(std::move(heading));
		const auto item = [&](const QString &text, const QByteArray &id) {
			auto result = Ui::MenuItem();
			result.text = text;
			result.checked = (chosen == id);
			result.callback = [=] {
				if (!weak) {
					return;
				} else if (input) {
					weak->setInputDevice(id);
				} else {
					weak->setOutputDevice(id);
				}
			};
			menu->addItem(std::move(result));
		};
		item(tr::lng_calls_device_default(tr::now), QByteArray());
		for (const auto &device : devices) {
			item(device.description(), device.id());
		}
		if (devices.isEmpty()) {
			auto none = Ui::MenuItem();
			none.text = tr::lng_calls_no_devices(tr::now);
			none.disabled = true;
			menu->addItem(std::move(none));
		}
	};
	add(
		tr::lng_calls_microphone(tr::now),
		QMediaDevices::audioInputs(),
		settings.inputDevice,
		true);
	menu->addSeparator();
	add(
		tr::lng_calls_speaker(tr::now),
		QMediaDevices::audioOutputs(),
		settings.outputDevice,
		false);
}

View::View(QWidget *parent, Service *service)
: RpWidget(parent)
, _service(service)
, _collapse(Ui::CreateChild<Ui::IconButton>(this, st::callsViewCollapse))
, _userpic(Ui::CreateChild<Support::UserpicButton>(
	this,
	service->session(),
	QString(),
	st::callsViewUserpic))
, _devices(Ui::CreateChild<LabeledButton>(
	this,
	st::callsViewDevices,
	tr::lng_calls_devices(tr::now)))
, _mute(Ui::CreateChild<LabeledButton>(
	this,
	st::callsViewMute,
	tr::lng_calls_mute(tr::now)))
, _end(Ui::CreateChild<LabeledButton>(
	this,
	st::callsViewEnd,
	tr::lng_calls_end(tr::now)))
, _again(Ui::CreateChild<LabeledButton>(
	this,
	st::callsViewAgain,
	tr::lng_calls_again(tr::now)))
, _close(Ui::CreateChild<LabeledButton>(
	this,
	st::callsViewClose,
	tr::lng_calls_close(tr::now)))
, _levels([this](qint64) {
	// The rings ease towards the last reported levels.
	const auto call = _service->call();
	const auto talking = call
		&& !call->ended()
		&& (call->state() == State::InCall);
	const auto output = talking ? double(_service->outputLevel()) : 0.;
	const auto input = (talking && !call->muted())
		? double(_service->inputLevel())
		: 0.;
	const auto step = [](double &value, double target) {
		// Up fast, down slowly: a voice, not a meter.
		const auto rate = (target > value) ? 0.45 : 0.12;
		value += (target - value) * rate;
		if (std::abs(target - value) < 0.004) {
			value = target;
		}
	};
	step(_outputRing, output);
	step(_inputRing, input);
	update();
	return isVisible() && (talking || _outputRing > 0. || _inputRing > 0.);
}) {
	setFocusPolicy(Qt::StrongFocus);
	Ui::MakeTransparentForMouse(_userpic);
	_userpic->setBorderColor(st::callsViewBg);

	_collapse->setClickedCallback([this] { _service->setExpanded(false); });
	_devices->setClickedCallback([this] { showDevicesMenu(); });
	_mute->setClickedCallback([this] { _service->toggleMuted(); });
	_end->setClickedCallback([this] { _service->hangup(); });
	_again->setClickedCallback([this] { _service->callAgain(); });
	_close->setClickedCallback([this] { _service->dismissEnded(); });
	Ui::InstallTooltip(_collapse, [] { return tr::lng_calls_collapse(tr::now); });

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
	connect(service, &Service::levelsChanged, this, [this] {
		if (Ui::AnimationsDisabled()) {
			_outputRing = _service->outputLevel();
			_inputRing = _service->inputLevel();
			update();
		} else if (!_levels.animating()) {
			_levels.start();
		}
	});
	refresh();
}

void View::refresh() {
	const auto call = _service->call();
	if (!call) {
		return;
	}
	if (_peerId != call->peerId()) {
		_peerId = call->peerId();
		_userpic->setUserId(_peerId);
	}
	_name = PeerName(_service->session(), _peerId);
	_mute->button()->setActive(call->muted());
	_mute->setLabel(call->muted()
		? tr::lng_calls_unmute(tr::now)
		: tr::lng_calls_mute(tr::now));
	if (call->state() == State::InCall && !call->ended()) {
		if (!_clock.isActive()) {
			_clock.start();
		}
		if (!_levels.animating() && !Ui::AnimationsDisabled()) {
			_levels.start();
		}
	} else {
		_clock.stop();
	}
	if (Ui::AnimationsDisabled()) {
		_outputRing = _service->outputLevel();
		_inputRing = (call->muted() ? 0.f : _service->inputLevel());
	}
	updateControls();
	update();
}

void View::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateControls();
}

void View::updateControls() {
	const auto call = _service->call();
	const auto ended = call && call->ended();
	const auto again = ended && CanCallAgain(*call);
	_devices->setVisible(!ended);
	_mute->setVisible(!ended);
	_end->setVisible(!ended);
	_again->setVisible(again);
	_close->setVisible(ended);
	// Nothing to go back to once the bar is gone as well.
	_collapse->setVisible(!ended);
	_collapse->moveToLeft(st::callsViewTopSkip, st::callsViewTopSkip, width());

	auto shown = std::vector<LabeledButton*>();
	for (const auto button : { _devices, _mute, _end, _again, _close }) {
		if (!button->isHidden()) {
			shown.push_back(button);
		}
	}
	if (shown.empty()) {
		return;
	}
	const auto skip = int(st::callsViewControlsSkip);
	auto total = -skip;
	for (const auto button : shown) {
		total += button->width() + skip;
	}
	const auto top = height()
		- st::callsViewControlsBottom
		- shown.front()->height();
	auto left = (width() - total) / 2;
	for (const auto button : shown) {
		button->moveToLeft(left, top, width());
		left += button->width() + skip;
	}
	const auto picture = int(st::callsViewUserpic);
	_userpic->move((width() - picture) / 2, contentTop());
}

int View::contentTop() const {
	// The picture, the name and the state line as one block, centred in the
	// space above the buttons.
	const auto block = st::callsViewUserpic
		+ st::callsViewNameTop
		+ st::callsViewNameFont->height
		+ st::callsViewStateTop
		+ st::callsViewDurationFont->height;
	const auto controlsTop = height()
		- st::callsViewControlsBottom
		- _mute->height();
	const auto ring = st::callsViewRingGap + st::callsViewRingMax;
	return std::max((controlsTop - block) / 2, int(st::callsViewTopSkip) + ring);
}

void View::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), st::callsViewBg);
	const auto call = _service->call();
	if (!call) {
		return;
	}
	const auto picture = _userpic->geometry();
	{
		// The voice rings: the other side's outside, the microphone's inside.
		auto hq = PainterHighQualityEnabler(p);
		const auto gap = double(st::callsViewRingGap);
		const auto ring = [&](double level, double inset, bool idle) {
			const auto width = st::callsViewRingMin
				+ (st::callsViewRingMax - st::callsViewRingMin) * level;
			const auto skip = gap + inset + width / 2.;
			auto pen = QPen((idle && level <= 0.)
				? st::callsViewRingIdleFg->c
				: st::callsViewRingFg->c);
			pen.setWidthF(width);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			p.setOpacity((level > 0.) ? (0.35 + 0.65 * level) : 1.);
			p.drawEllipse(QRectF(picture).marginsAdded(
				QMarginsF(skip, skip, skip, skip)));
		};
		ring(std::clamp(_outputRing, 0., 1.), 0., true);
		if (_inputRing > 0.02) {
			p.setOpacity(1.);
			auto pen = QPen(st::callsViewRingFg->c);
			pen.setWidthF(1.5);
			p.setPen(pen);
			p.setOpacity(0.25 + 0.5 * std::clamp(_inputRing, 0., 1.));
			const auto skip = gap / 2.;
			p.drawEllipse(QRectF(picture).marginsAdded(
				QMarginsF(skip, skip, skip, skip)));
		}
		p.setOpacity(1.);
	}

	const auto padding = int(st::callsViewNotePadding);
	const auto available = width() - 2 * padding;
	auto top = picture.y() + picture.height() + st::callsViewNameTop;
	const auto name = _name.isEmpty()
		? tr::lng_calls_voice_call(tr::now)
		: _name;
	p.setFont(st::callsViewNameFont);
	p.setPen(st::callsViewNameFg);
	p.drawTextCentered(
		QRect(padding, top, available, st::callsViewNameFont->height),
		st::callsViewNameFont->elided(name, available));
	top += st::callsViewNameFont->height + st::callsViewStateTop;

	const auto talking = !call->ended() && (call->state() == State::InCall);
	const auto &font = talking
		? st::callsViewDurationFont
		: st::callsViewStateFont;
	p.setFont(font);
	p.setPen(talking
		? st::callsViewDurationFg
		: StateIsWarning(*call)
		? st::callsViewWarnFg
		: st::callsViewStateFg);
	p.drawTextCentered(
		QRect(padding, top, available, st::callsViewDurationFont->height),
		font->elided(
			StateText(*call, QDateTime::currentMSecsSinceEpoch()),
			available));
	top += st::callsViewDurationFont->height + st::callsViewHintTop;

	// One more line when there is something to add.
	auto hint = QString();
	if (!call->ended() && call->state() == State::Reconnecting) {
		hint = tr::lng_calls_weak_network(tr::now);
	} else if (!call->ended() && call->muted()) {
		hint = tr::lng_calls_muted(tr::now);
	}
	if (!hint.isEmpty()) {
		p.setFont(st::callsViewHintFont);
		p.setPen(st::callsViewNameFg);
		p.setOpacity(0.8);
		p.drawTextCentered(
			QRect(padding, top, available, st::callsViewHintFont->height),
			st::callsViewHintFont->elided(hint, available));
		p.setOpacity(1.);
	}

	// Calls are relayed by the server and are not end-to-end encrypted:
	// said where the call is.
	p.setFont(st::callsViewNoteFont);
	p.setPen(st::callsViewNoteFg);
	p.setOpacity(0.6);
	p.drawTextCentered(
		QRect(
			padding,
			height() - st::callsViewNoteBottom - st::callsViewNoteFont->height,
			available,
			st::callsViewNoteFont->height),
		st::callsViewNoteFont->elided(
			tr::lng_calls_privacy_note(tr::now),
			available));
}

void View::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Back) {
		const auto call = _service->call();
		if (call && call->ended()) {
			_service->dismissEnded();
		} else {
			_service->setExpanded(false);
		}
	} else {
		RpWidget::keyPressEvent(e);
	}
}

void View::mousePressEvent(QMouseEvent *e) {
	// Clicks do not fall through to what is under the call.
	Q_UNUSED(e);
	setFocus();
}

void View::showDevicesMenu() {
	if (_menu) {
		_menu->hideMenu(true);
	}
	_menu = new Ui::PopupMenu(this);
	FillDevicesMenu(_menu, _service);
	_menu->popupAnchored(_devices->button(), style::al_center);
}

} // namespace Calls
