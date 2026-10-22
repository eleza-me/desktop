#include "calls/calls_sounds.h"

#include <QtCore/QUrl>
#include <QtMultimedia/QSoundEffect>

namespace Calls {
namespace {

constexpr auto kToneVolume = 0.55;

[[nodiscard]] QUrl SoundUrl(const QString &name) {
	return QUrl(QStringLiteral("qrc:/calls/sounds/") + name + QStringLiteral(".wav"));
}

[[nodiscard]] std::unique_ptr<QSoundEffect> Make(const QString &name) {
	auto result = std::make_unique<QSoundEffect>();
	result->setSource(SoundUrl(name));
	return result;
}

} // namespace

Sounds::Sounds(QObject *parent)
: QObject(parent) {
}

Sounds::~Sounds() = default;

void Sounds::setRingtoneVolume(double volume) {
	const auto was = _ringtoneVolume;
	_ringtoneVolume = std::clamp(volume, 0., 1.);
	if (_ringtone) {
		_ringtone->setVolume(float(_ringtoneVolume));
	}
	// Ringing began silent, so the ringtone was never started.
	if (_loop == Loop::Ringtone
		&& _enabled
		&& was <= 0.
		&& _ringtoneVolume > 0.
		&& (!_ringtone || !_ringtone->isPlaying())) {
		_loop = Loop::None;
		loop(Loop::Ringtone);
	}
}

void Sounds::setEnabled(bool enabled) {
	if (_enabled == enabled) {
		return;
	}
	_enabled = enabled;
	if (!enabled) {
		const auto was = _loop;
		stop();
		_loop = was;
	} else if (_loop != Loop::None) {
		const auto was = std::exchange(_loop, Loop::None);
		loop(was);
	}
}

void Sounds::loop(Loop what) {
	if (_loop == what) {
		return;
	}
	stop();
	_loop = what;
	if (!_enabled || what == Loop::None) {
		return;
	}
	if (what == Loop::Ringtone) {
		if (_ringtoneVolume <= 0.) {
			return;
		}
		if (!_ringtone) {
			_ringtone = Make(QStringLiteral("call_ringtone"));
		}
		_ringtone->setVolume(float(_ringtoneVolume));
		_ringtone->setLoopCount(QSoundEffect::Infinite);
		_ringtone->play();
	} else {
		if (!_ringback) {
			_ringback = Make(QStringLiteral("call_ringback"));
			_ringback->setVolume(float(kToneVolume));
		}
		_ringback->setLoopCount(QSoundEffect::Infinite);
		_ringback->play();
	}
}

void Sounds::play(Tone tone) {
	stop();
	if (!_enabled) {
		return;
	}
	const auto name = (tone == Tone::Connect)
		? QStringLiteral("call_connect")
		: (tone == Tone::Busy)
		? QStringLiteral("call_busy")
		: QStringLiteral("call_end");
	if (!_tone || _toneName != name) {
		_tone = Make(name);
		_toneName = name;
		_tone->setVolume(float(kToneVolume));
	}
	_tone->setLoopCount(1);
	_tone->play();
}

void Sounds::stop() {
	_loop = Loop::None;
	for (const auto effect : { _ringtone.get(), _ringback.get(), _tone.get() }) {
		if (effect && effect->isPlaying()) {
			effect->stop();
		}
	}
}

void Sounds::preview() {
	if (_loop != Loop::None || !_enabled || _ringtoneVolume <= 0.) {
		return;
	}
	if (!_ringtone) {
		_ringtone = Make(QStringLiteral("call_ringtone"));
	}
	_ringtone->stop();
	_ringtone->setVolume(float(_ringtoneVolume));
	_ringtone->setLoopCount(1);
	_ringtone->play();
}

} // namespace Calls
