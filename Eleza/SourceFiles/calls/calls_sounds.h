// The sounds of a call (Resources/sounds/call_*.wav, made by
// codegen/scripts/gen_call_sounds.py):
//
//   ringtone   loops while a call rings here; its volume is the user's
//              «بلندی صدای زنگ», and it is silent when the app's sounds are off
//   ringback   loops while my call rings on their device
//   connect / end / busy   short tones when the call connects and when it ends
//
// One thing plays at a time. Nothing plays in a screenshot run.
#pragma once

#include <QtCore/QObject>

#include <memory>

class QSoundEffect;

namespace Calls {

class Sounds final : public QObject {
public:
	enum class Loop {
		None,
		Ringtone,
		Ringback,
	};
	enum class Tone {
		Connect,
		End,
		Busy,
	};

	explicit Sounds(QObject *parent = nullptr);
	~Sounds();

	// 0..1 for the ringtone (0: it rings silently).
	void setRingtoneVolume(double volume);
	void setEnabled(bool enabled);

	void loop(Loop what);
	[[nodiscard]] Loop looping() const {
		return _loop;
	}
	// Stops a loop and plays the tone once.
	void play(Tone tone);
	void stop();
	// The ringtone once, for the volume slider of the settings.
	void preview();

private:
	[[nodiscard]] QSoundEffect *effect(const QString &name);

	std::unique_ptr<QSoundEffect> _ringtone;
	std::unique_ptr<QSoundEffect> _ringback;
	std::unique_ptr<QSoundEffect> _tone;
	QString _toneName;
	Loop _loop = Loop::None;
	double _ringtoneVolume = 0.7;
	bool _enabled = true;

};

} // namespace Calls
