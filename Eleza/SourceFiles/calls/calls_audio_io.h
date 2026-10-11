// Microphone in, loudspeaker out, for the media thread of a call.
//
// Both sides of the media library are 48 kHz mono 16-bit in frames of 10 ms:
//
//   Input   QAudioSource in whatever format the device takes -> 10 ms frames
//   Output  10 ms frames -> a short queue -> QAudioSink (pull mode)
//
// The output also keeps what the device has actually taken: the reference
// the echo canceller compares the microphone with (takeReference()).
//
// With the debug tools both ends can be replaced, so that two copies of the
// app can talk on a machine without sound hardware (tests/tools/calls-e2e.sh):
// a generated tone instead of the microphone, a .wav recorder instead of the
// loudspeaker. Timing of both follows the clock, like a device would.
//
// The objects live on one thread (the media thread) with an event loop; the
// sink may read from a thread of its own, which is why the queue is locked.
// The pure parts (format conversion, the queue, levels) are unit-tested in
// tests/app/test_calls_audio.cpp.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <QtCore/QMutex>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QAudioFormat>

#include <functional>
#include <memory>
#include <vector>

class QAudioSink;
class QAudioSource;

namespace Calls::Audio {

inline constexpr auto kSampleRate = 48000;
inline constexpr auto kFrameMs = 10;
inline constexpr auto kFrameSamples = kSampleRate * kFrameMs / 1000; // 480

// The device with this id among `devices`, or `fallback` (the default one)
// when the id is empty or not there any more; `found` tells which.
[[nodiscard]] QAudioDevice PickDevice(
	const QList<QAudioDevice> &devices,
	const QAudioDevice &fallback,
	const QByteArray &id,
	bool *found = nullptr);

// 0..1 for a meter: silence .. full scale on a decibel scale (-60 dB .. 0).
[[nodiscard]] float Level(const qint16 *samples, int count);

// Interleaved samples of `format` -> mono 16-bit at the same rate.
void ToMono16(
	const char *data,
	qsizetype bytes,
	const QAudioFormat &format,
	std::vector<qint16> &out);
// Mono 16-bit -> interleaved samples of `format` (the same rate), appended.
void FromMono16(
	const qint16 *samples,
	int count,
	const QAudioFormat &format,
	QByteArray &out);

// Linear interpolation between rates: the fallback for a device that does
// not take 48 kHz (the audio servers of both platforms normally do).
class Resampler {
public:
	Resampler(int fromRate, int toRate);

	void push(const qint16 *samples, int count, std::vector<qint16> &out);

private:
	double _step = 1.;
	double _position = 0.;
	qint16 _last = 0;
	bool _primed = false;

};

// The queue between the network and the loudspeaker. The library already
// smooths the network's jitter; this absorbs only the uneven timing of two
// threads: it starts playing with 30 ms queued, plays silence when empty and
// drops the oldest audio beyond 200 ms so that latency cannot pile up.
class PlayoutQueue {
public:
	void push(const qint16 *samples, int count);
	// Always fills `count` samples (silence when there is not enough).
	void pull(qint16 *samples, int count);
	void clear();
	[[nodiscard]] int queuedMs() const;

	static constexpr auto kStartMs = 30;
	static constexpr auto kMaxMs = 200;
	static constexpr auto kTrimToMs = 80;

private:
	std::vector<qint16> _samples;
	qsizetype _head = 0;
	bool _playing = false;

};

class Input final : public QObject {
public:
	// Called on the object's thread with one frame of kFrameSamples; the
	// samples may be changed in place.
	using FrameCallback = std::function<void(qint16 *samples)>;

	explicit Input(QObject *parent = nullptr);
	~Input();

	void setFrameCallback(FrameCallback callback);
	// The device stopped by itself (unplugged, refused).
	void setFailedCallback(std::function<void()> callback);

	[[nodiscard]] bool start(const QAudioDevice &device);
#ifdef ELEZA_DEBUG_TOOLS
	// "tone:<Hz>": the tone sounds during the first half of every second of
	// the system clock, so a recording of it shows the delay of the path.
	[[nodiscard]] bool startGenerated(const QString &spec);
#endif // ELEZA_DEBUG_TOOLS
	void stop();
	[[nodiscard]] bool active() const {
		return _active;
	}
	// Roughly how old a sample is when the callback sees it.
	[[nodiscard]] int delayMs() const;

private:
	void readDevice();
	void emitFrames();
#ifdef ELEZA_DEBUG_TOOLS
	void generate();
#endif // ELEZA_DEBUG_TOOLS

	FrameCallback _frame;
	std::function<void()> _failed;
	std::unique_ptr<QAudioSource> _source;
	QIODevice *_io = nullptr;
	QAudioFormat _format;
	QByteArray _raw;
	std::vector<qint16> _mono;
	std::vector<qint16> _ready;
	std::unique_ptr<Resampler> _resampler;
	bool _active = false;
#ifdef ELEZA_DEBUG_TOOLS
	QTimer _generator;
	QElapsedTimer _clock;
	qint64 _startedAtMs = 0;
	qint64 _generated = 0; // samples
	double _toneHz = 0.;
#endif // ELEZA_DEBUG_TOOLS

};

class Output final : public QObject {
public:
	explicit Output(QObject *parent = nullptr);
	~Output();

	[[nodiscard]] bool start(const QAudioDevice &device);
#ifdef ELEZA_DEBUG_TOOLS
	// Records what would be played into a .wav file (48 kHz mono) and writes
	// <path>.json with the clock time of its first sample.
	[[nodiscard]] bool startRecording(const QString &path);
#endif // ELEZA_DEBUG_TOOLS
	void stop();
	[[nodiscard]] bool active() const {
		return _active;
	}

	// One or more frames from the other side.
	void push(const qint16 *samples, int count);
	// What the device took since the last call, in order (48 kHz mono).
	void takeReference(std::vector<qint16> &out);
	// How long a sample waits between takeReference() and the loudspeaker.
	[[nodiscard]] int delayMs() const;
	// 0..1 of what was played lately.
	[[nodiscard]] float takeLevel();

private:
	class Device;
	friend class Device;

	// Called by the sink (any thread): `count` samples at 48 kHz mono.
	void consume(qint16 *samples, int count);
#ifdef ELEZA_DEBUG_TOOLS
	void record();
	void finishRecording();
#endif // ELEZA_DEBUG_TOOLS

	mutable QMutex _mutex;
	PlayoutQueue _queue;
	std::vector<qint16> _reference;
	float _level = 0.;
	std::unique_ptr<QAudioSink> _sink;
	std::unique_ptr<Device> _device;
	QAudioFormat _format;
	bool _active = false;
#ifdef ELEZA_DEBUG_TOOLS
	QFile _file;
	QTimer _recorder;
	QElapsedTimer _clock;
	qint64 _recorded = 0; // samples
#endif // ELEZA_DEBUG_TOOLS

};

} // namespace Calls::Audio
