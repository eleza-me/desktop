// «تماس‌ها» in the settings: the microphone and the speaker of calls with a
// microphone test, noise suppression / echo cancellation, the volume of the
// ringtone (all of this computer: Core::Settings::calls()) and who may call
// me (the account's privacy.calls, synced like the other privacy choices).
#include "calls/calls_settings.h"

#include "calls/calls_audio_io.h"
#include "calls/calls_media.h"
#include "calls/calls_service.h"
#include "calls/calls_video.h"
#include "calls/calls_sounds.h"
#include "core/core_application.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "settings/settings_logic.h"
#include "settings/settings_pages.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_calls.h"
#include "styles/style_settings.h"

#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QMediaDevices>

namespace Calls {
namespace {

constexpr auto kMicTestSeconds = 20;

[[nodiscard]] QString DeviceValue(const QByteArray &id) {
	return QString::fromLatin1(id.toBase64());
}

[[nodiscard]] QByteArray DeviceId(const QString &value) {
	return QByteArray::fromBase64(value.toLatin1());
}

[[nodiscard]] QString DeviceTitle(
		const QList<QAudioDevice> &devices,
		const QByteArray &id) {
	if (id.isEmpty()) {
		return tr::lng_calls_device_default(tr::now);
	}
	for (const auto &device : devices) {
		if (device.id() == id) {
			return device.description();
		}
	}
	// Chosen once and not plugged in now: the default is used meanwhile.
	return tr::lng_calls_device_default(tr::now);
}

// The level of the microphone as a bar, with a button that listens for a
// while. During a call it shows the call's own microphone.
class MicMeter final : public Ui::RpWidget {
public:
	MicMeter(QWidget *parent, Main::Session *session)
	: RpWidget(parent)
	, _session(session)
	, _button(Ui::CreateChild<Ui::RoundButton>(
		this,
		tr::lng_calls_settings_mic_test(tr::now),
		st::callsSettingsMeterButton)) {
		_button->setClickedCallback([this] { toggle(); });
		_stop.setSingleShot(true);
		connect(&_stop, &QTimer::timeout, this, [this] { stop(); });
		// The bar falls back smoothly between two reports.
		_decay.setInterval(60);
		connect(&_decay, &QTimer::timeout, this, [this] {
			const auto target = inCall() ? callLevel() : _peak;
			_peak = std::max(_peak - 0.08f, 0.f);
			_shown += (target - _shown) * 0.5f;
			if (!_input && !inCall() && _shown < 0.01f) {
				_shown = 0.f;
				_decay.stop();
			}
			update();
		});
		if (const auto service = Service::For(session)) {
			connect(service, &Service::callUpdated, this, [this] { refresh(); });
			connect(service, &Service::callChanged, this, [this] { refresh(); });
		}
		refresh();
	}

	~MicMeter() {
		stop();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto &padding = st::callsSettingsMeterPadding;
		_button->moveToRight(
			padding.right(),
			(st::callsSettingsMeterHeight - _button->height()) / 2,
			newWidth);
		return st::callsSettingsMeterHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto &padding = st::callsSettingsMeterPadding;
		const auto &font = st::callsSettingsMeterLabelFont;
		const auto right = padding.right()
			+ (_button->isHidden() ? 0 : (_button->width() + int(st::spaceLg)));
		const auto available = width() - padding.left() - right;
		p.setFont(font);
		p.setPen(st::callsSettingsMeterLabelFg);
		p.drawTextLeft(
			padding.left(),
			10,
			width(),
			font->elided(_failed
				? tr::lng_calls_microphone_failed(tr::now)
				: (_input || inCall())
				? tr::lng_calls_settings_mic_test_about(tr::now)
				: tr::lng_calls_settings_mic_level(tr::now), available));
		auto hq = PainterHighQualityEnabler(p);
		const auto line = int(st::callsSettingsMeterLine);
		const auto bar = QRect(
			padding.left(),
			10 + font->height + 10,
			available,
			line);
		p.setPen(Qt::NoPen);
		p.setBrush(st::callsSettingsMeterBg);
		p.drawRoundedRect(myrtlrect(bar), line / 2., line / 2.);
		const auto filled = int(std::round(bar.width() * std::clamp(_shown, 0.f, 1.f)));
		if (filled > 0) {
			p.setBrush(st::callsSettingsMeterFg);
			p.drawRoundedRect(
				myrtlrect(bar.x(), bar.y(), std::max(filled, line), line),
				line / 2.,
				line / 2.);
		}
	}

	void hideEvent(QHideEvent *e) override {
		stop();
		RpWidget::hideEvent(e);
	}

private:
	[[nodiscard]] bool inCall() const {
		const auto service = Service::For(_session);
		return service && service->engaged();
	}
	[[nodiscard]] float callLevel() const {
		const auto service = Service::For(_session);
		return service ? service->inputLevel() : 0.f;
	}

	void refresh() {
		// The call has the microphone: no second capture next to it.
		const auto busy = inCall();
		if (busy && _input) {
			stop();
		}
		_button->setVisible(!busy);
		if (busy && !_decay.isActive()) {
			_decay.start();
		}
		update();
	}

	void toggle() {
		if (_input) {
			stop();
		} else {
			start();
		}
	}

	void start() {
		_failed = false;
		const auto &settings = Core::App().settings().calls();
		const auto device = Audio::PickDevice(
			QMediaDevices::audioInputs(),
			QMediaDevices::defaultAudioInput(),
			settings.inputDevice);
		_input = std::make_unique<Audio::Input>();
		_input->setFrameCallback([this](qint16 *samples) {
			_peak = std::max(_peak, Audio::Level(samples, Audio::kFrameSamples));
		});
		_input->setFailedCallback([this] {
			_failed = true;
			stop();
		});
		if (!_input->start(device)) {
			_input = nullptr;
			_failed = true;
			update();
			return;
		}
		_button->setText(tr::lng_calls_settings_mic_test_stop(tr::now));
		resizeToWidth(width());
		_stop.start(kMicTestSeconds * 1000);
		_decay.start();
		update();
	}

	void stop() {
		_stop.stop();
		if (!_input) {
			return;
		}
		_input = nullptr;
		_peak = 0.f;
		_button->setText(tr::lng_calls_settings_mic_test(tr::now));
		resizeToWidth(width());
		update();
	}

	Main::Session *_session = nullptr;
	Ui::RoundButton *_button = nullptr;
	std::unique_ptr<Audio::Input> _input;
	QTimer _stop;
	QTimer _decay;
	float _peak = 0.f;
	float _shown = 0.f;
	bool _failed = false;

};

// 0..1, always left to right (like every level control).
class VolumeSlider final : public Ui::RpWidget {
public:
	VolumeSlider(
		QWidget *parent,
		double value,
		std::function<void(double)> changed,
		std::function<void()> released)
	: RpWidget(parent)
	, _value(std::clamp(value, 0., 1.))
	, _changed(std::move(changed))
	, _released(std::move(released)) {
		setCursor(style::cur_pointer);
		setFocusPolicy(Qt::TabFocus);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return st::callsSettingsSliderHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto &padding = st::defaultSettingsButton.padding;
		st::callsSettingsVolumeIcon.paint(
			p,
			padding.left(),
			(height() - st::callsSettingsVolumeIcon.height()) / 2,
			width());
		const auto track = trackRect();
		const auto line = double(st::callsSettingsSliderLine);
		const auto y = track.y() + (track.height() - line) / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(st::callsSettingsSliderBg);
		p.drawRoundedRect(
			QRectF(track.x(), y, track.width(), line),
			line / 2.,
			line / 2.);
		const auto knob = double(st::callsSettingsSliderKnob);
		const auto center = track.x() + knob / 2. + (track.width() - knob) * _value;
		p.setBrush(st::callsSettingsSliderFg);
		p.drawRoundedRect(
			QRectF(track.x(), y, center - track.x(), line),
			line / 2.,
			line / 2.);
		p.drawEllipse(QPointF(center, track.y() + track.height() / 2.), knob / 2., knob / 2.);

		const auto &font = st::callsSettingsSliderValueFont;
		p.setFont(font);
		p.setPen(st::callsSettingsSliderValueFg);
		const auto text = tr::lng_calls_settings_percent(
			tr::now,
			lt_value,
			Lang::Num(int(std::round(_value * 100.))));
		p.drawTextRight(
			padding.right(),
			(height() - font->height) / 2,
			width(),
			text);
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton) {
			_pressed = true;
			setFromPosition(e->pos().x());
		}
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (_pressed) {
			setFromPosition(e->pos().x());
		}
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		Q_UNUSED(e);
		if (std::exchange(_pressed, false) && _released) {
			_released();
		}
	}
	void keyPressEvent(QKeyEvent *e) override {
		if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right) {
			set(_value + ((e->key() == Qt::Key_Right) ? 0.05 : -0.05));
			if (_released) {
				_released();
			}
		} else {
			RpWidget::keyPressEvent(e);
		}
	}

private:
	// The track between the icon and the percentage, in physical
	// coordinates: louder is to the right in both languages.
	[[nodiscard]] QRect trackRect() const {
		const auto &padding = st::defaultSettingsButton.padding;
		const auto leading = padding.left() + int(st::callsSettingsSliderIconSkip);
		const auto trailing = padding.right()
			+ int(st::callsSettingsSliderValueWidth);
		const auto left = style::RightToLeft() ? trailing : leading;
		const auto right = style::RightToLeft() ? leading : trailing;
		return QRect(left, 0, std::max(width() - left - right, 1), height());
	}
	void setFromPosition(int x) {
		const auto track = trackRect();
		const auto knob = double(st::callsSettingsSliderKnob);
		set((x - track.x() - knob / 2.) / std::max(track.width() - knob, 1.));
	}
	void set(double value) {
		value = std::clamp(value, 0., 1.);
		if (_value != value) {
			_value = value;
			if (_changed) {
				_changed(value);
			}
			update();
		}
	}

	double _value = 0.;
	std::function<void(double)> _changed;
	std::function<void()> _released;
	bool _pressed = false;

};

class Page final : public Settings::SyncedPage {
public:
	Page(QWidget *parent, Window::SessionController *controller)
	: SyncedPage(parent, controller, Settings::Section::Calls) {
		buildLocal();
		showContent();
		loadInline();
	}

private:
	void pickDevice(bool input) {
		const auto devices = input
			? QMediaDevices::audioInputs()
			: QMediaDevices::audioOutputs();
		const auto &settings = Core::App().settings().calls();
		const auto current = input ? settings.inputDevice : settings.outputDevice;
		auto options = std::vector<Settings::Option>();
		options.push_back({ QString(), tr::lng_calls_device_default(tr::now) });
		auto known = current.isEmpty();
		for (const auto &device : devices) {
			known = known || (device.id() == current);
			options.push_back({ DeviceValue(device.id()), device.description() });
		}
		const auto session = &controller()->session();
		Settings::ShowPickerBox(
			controller(),
			input
				? tr::lng_calls_microphone(tr::now)
				: tr::lng_calls_speaker(tr::now),
			options,
			known ? DeviceValue(current) : QString(),
			Settings::Guarded(this, [=, this](const QString &value) {
				const auto id = DeviceId(value);
				if (const auto service = Service::For(session)) {
					if (input) {
						service->setInputDevice(id);
					} else {
						service->setOutputDevice(id);
					}
				}
				refreshDevices();
			}));
	}

	void pickCamera() {
		const auto current = Core::App().settings().calls().cameraDevice;
		auto options = std::vector<Settings::Option>();
		options.push_back({ QString(), tr::lng_calls_device_default(tr::now) });
		auto known = current.isEmpty();
		for (const auto &camera : Video::Cameras()) {
			known = known || (camera.id == current);
			options.push_back({ DeviceValue(camera.id), camera.name });
		}
		const auto session = &controller()->session();
		Settings::ShowPickerBox(
			controller(),
			tr::lng_calls_settings_camera(tr::now),
			options,
			known ? DeviceValue(current) : QString(),
			Settings::Guarded(this, [=, this](const QString &value) {
				if (const auto service = Service::For(session)) {
					service->setCameraDevice(DeviceId(value));
				}
				refreshDevices();
			}));
	}

	void refreshDevices() {
		const auto &settings = Core::App().settings().calls();
		if (_camera) {
			const auto cameras = Video::Cameras();
			auto title = cameras.isEmpty()
				? tr::lng_calls_no_cameras(tr::now)
				: tr::lng_calls_device_default(tr::now);
			for (const auto &camera : cameras) {
				if (camera.id == settings.cameraDevice) {
					title = camera.name;
				}
			}
			_camera->setValueText(title);
		}
		if (_microphone) {
			_microphone->setValueText(DeviceTitle(
				QMediaDevices::audioInputs(),
				settings.inputDevice));
		}
		if (_speaker) {
			_speaker->setValueText(DeviceTitle(
				QMediaDevices::audioOutputs(),
				settings.outputDevice));
		}
	}

	// What this computer does; kept on this computer.
	void buildLocal() {
		if (CheckMedia() != MediaProblem::None) {
			Settings::AddFootnote(content(), tr::lng_calls_unavailable(tr::now));
			Settings::AddDivider(content());
		}
		Settings::AddSubsectionTitle(
			content(),
			tr::lng_calls_settings_devices(tr::now));
		_microphone = Settings::AddLinkButton(
			content(),
			tr::lng_calls_microphone(tr::now),
			&st::callsSettingsMicIcon,
			[this] { pickDevice(true); });
		_speaker = Settings::AddLinkButton(
			content(),
			tr::lng_calls_speaker(tr::now),
			&st::callsSettingsSpeakerIcon,
			[this] { pickDevice(false); });
		const auto service = Service::For(&controller()->session());
		if (service && service->videoOffered()) {
			// (Only where the server has video calls.)
			_camera = Settings::AddLinkButton(
				content(),
				tr::lng_calls_settings_camera(tr::now),
				&st::callsSettingsCameraIcon,
				[this] { pickCamera(); });
			connect(&_devices, &QMediaDevices::videoInputsChanged, this, [this] {
				refreshDevices();
			});
		}
		content()->add(Ui::object_ptr<MicMeter>(
			content(),
			&controller()->session()));
		connect(&_devices, &QMediaDevices::audioInputsChanged, this, [this] {
			refreshDevices();
		});
		connect(&_devices, &QMediaDevices::audioOutputsChanged, this, [this] {
			refreshDevices();
		});
		// The device menu of a running call changes the same settings.
		connect(&Core::App().settings(), &Core::Settings::changed, this, [this] {
			refreshDevices();
		});
		refreshDevices();

		Settings::AddDivider(content());
		Settings::AddSubsectionTitle(
			content(),
			tr::lng_calls_settings_processing(tr::now));
		Settings::AddToggle(
			content(),
			tr::lng_calls_settings_noise(tr::now),
			Core::App().settings().calls().noiseSuppression,
			[](bool checked) {
				auto settings = Core::App().settings().calls();
				settings.noiseSuppression = checked;
				Core::App().settings().setCalls(settings);
			});
		Settings::AddToggle(
			content(),
			tr::lng_calls_settings_echo(tr::now),
			Core::App().settings().calls().echoCancellation,
			[](bool checked) {
				auto settings = Core::App().settings().calls();
				settings.echoCancellation = checked;
				Core::App().settings().setCalls(settings);
			});
		Settings::AddFootnote(
			content(),
			tr::lng_calls_settings_processing_about(tr::now));

		Settings::AddDivider(content());
		Settings::AddSubsectionTitle(
			content(),
			tr::lng_calls_settings_ringtone(tr::now));
		const auto session = &controller()->session();
		content()->add(Ui::object_ptr<VolumeSlider>(
			content(),
			Core::App().settings().calls().ringtoneVolume,
			[](double value) {
				auto settings = Core::App().settings().calls();
				settings.ringtoneVolume = value;
				Core::App().settings().setCalls(settings);
			},
			[=] {
				// What it sounds like now.
				if (const auto service = Service::For(session)) {
					service->sounds()->preview();
				}
			}));
		Settings::AddFootnote(
			content(),
			tr::lng_calls_settings_ringtone_about(tr::now));
		Settings::AddDivider(content());
	}

	// The account's own choice, the same on every device.
	void build() override {
		Settings::AddSubsectionTitle(
			content(),
			tr::lng_settings_who_can(tr::now));
		addPicker(
			tr::lng_settings_who_call(tr::now),
			u"privacy.calls"_s,
			{
				{
					u"thread_contacts"_s,
					tr::lng_settings_audience_chat_people(tr::now),
				},
				{ u"nobody"_s, tr::lng_settings_audience_nobody(tr::now) },
			},
			u"thread_contacts"_s);
		Settings::AddFootnote(content(), tr::lng_calls_privacy_note(tr::now));
	}

	QMediaDevices _devices;
	QPointer<Ui::SettingsButton> _microphone;
	QPointer<Ui::SettingsButton> _speaker;
	QPointer<Ui::SettingsButton> _camera;

};

} // namespace

void RegisterSettingsPage() {
	Settings::RegisterCallsPage([](
			QWidget *parent,
			Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(new Page(parent, controller));
	});
}

} // namespace Calls
