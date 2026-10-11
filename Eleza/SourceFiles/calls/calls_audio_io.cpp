#include "calls/calls_audio_io.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QtEndian>
#include <QtMultimedia/QAudioSink>
#include <QtMultimedia/QAudioSource>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Calls::Audio {
namespace {

constexpr auto kMaxReferenceMs = 500;
constexpr auto kPi = 3.14159265358979323846;

[[nodiscard]] QAudioFormat PreferredFormat(int channels) {
	auto result = QAudioFormat();
	result.setSampleRate(kSampleRate);
	result.setChannelCount(channels);
	result.setSampleFormat(QAudioFormat::Int16);
	return result;
}

[[nodiscard]] bool Usable(const QAudioFormat &format) {
	return format.isValid()
		&& format.sampleRate() >= 8000
		&& format.sampleRate() <= 192000
		&& format.channelCount() >= 1
		&& format.channelCount() <= 8
		&& format.sampleFormat() != QAudioFormat::Unknown;
}

[[nodiscard]] qint16 Clip(double value) {
	return qint16(std::clamp(value, -32768., 32767.));
}

#ifdef ELEZA_DEBUG_TOOLS
[[nodiscard]] QByteArray WavHeader(quint32 dataBytes) {
	auto result = QByteArray(44, '\0');
	const auto put32 = [&](int at, quint32 value) {
		qToLittleEndian(value, result.data() + at);
	};
	const auto put16 = [&](int at, quint16 value) {
		qToLittleEndian(value, result.data() + at);
	};
	std::memcpy(result.data(), "RIFF", 4);
	put32(4, 36 + dataBytes);
	std::memcpy(result.data() + 8, "WAVEfmt ", 8);
	put32(16, 16);
	put16(20, 1); // PCM
	put16(22, 1); // mono
	put32(24, kSampleRate);
	put32(28, kSampleRate * 2);
	put16(32, 2);
	put16(34, 16);
	std::memcpy(result.data() + 36, "data", 4);
	put32(40, dataBytes);
	return result;
}
#endif // ELEZA_DEBUG_TOOLS

} // namespace

QAudioDevice PickDevice(
		const QList<QAudioDevice> &devices,
		const QAudioDevice &fallback,
		const QByteArray &id,
		bool *found) {
	if (found) {
		*found = true;
	}
	if (id.isEmpty()) {
		return fallback;
	}
	for (const auto &device : devices) {
		if (device.id() == id) {
			return device;
		}
	}
	if (found) {
		*found = false;
	}
	return fallback;
}

float Level(const qint16 *samples, int count) {
	if (count <= 0) {
		return 0.;
	}
	auto sum = 0.;
	for (auto i = 0; i != count; ++i) {
		const auto value = double(samples[i]) / 32768.;
		sum += value * value;
	}
	const auto rms = std::sqrt(sum / count);
	if (rms <= 0.001) { // -60 dB
		return 0.;
	}
	const auto decibels = 20. * std::log10(rms);
	return float(std::clamp((decibels + 60.) / 60., 0., 1.));
}

void ToMono16(
		const char *data,
		qsizetype bytes,
		const QAudioFormat &format,
		std::vector<qint16> &out) {
	const auto channels = format.channelCount();
	const auto bytesPerSample = format.bytesPerSample();
	if (channels <= 0 || bytesPerSample <= 0) {
		return;
	}
	const auto frames = bytes / (qsizetype(channels) * bytesPerSample);
	out.reserve(out.size() + size_t(frames));
	auto at = data;
	for (auto i = qsizetype(); i != frames; ++i) {
		auto sum = 0.;
		for (auto channel = 0; channel != channels; ++channel) {
			switch (format.sampleFormat()) {
			case QAudioFormat::UInt8:
				sum += (double(uchar(*at)) - 128.) * 256.;
				break;
			case QAudioFormat::Int16: {
				auto value = qint16();
				std::memcpy(&value, at, sizeof(value));
				sum += value;
			} break;
			case QAudioFormat::Int32: {
				auto value = qint32();
				std::memcpy(&value, at, sizeof(value));
				sum += double(value) / 65536.;
			} break;
			case QAudioFormat::Float: {
				auto value = float();
				std::memcpy(&value, at, sizeof(value));
				sum += double(value) * 32767.;
			} break;
			default: break;
			}
			at += bytesPerSample;
		}
		out.push_back(Clip(sum / channels));
	}
}

void FromMono16(
		const qint16 *samples,
		int count,
		const QAudioFormat &format,
		QByteArray &out) {
	const auto channels = format.channelCount();
	const auto bytesPerSample = format.bytesPerSample();
	if (channels <= 0 || bytesPerSample <= 0 || count <= 0) {
		return;
	}
	const auto was = out.size();
	out.resize(was + qsizetype(count) * channels * bytesPerSample);
	auto at = out.data() + was;
	for (auto i = 0; i != count; ++i) {
		const auto sample = samples[i];
		for (auto channel = 0; channel != channels; ++channel) {
			switch (format.sampleFormat()) {
			case QAudioFormat::UInt8:
				*at = char(uchar((int(sample) >> 8) + 128));
				break;
			case QAudioFormat::Int16:
				std::memcpy(at, &sample, sizeof(sample));
				break;
			case QAudioFormat::Int32: {
				const auto value = qint32(sample) * 65536;
				std::memcpy(at, &value, sizeof(value));
			} break;
			case QAudioFormat::Float: {
				const auto value = float(sample) / 32768.f;
				std::memcpy(at, &value, sizeof(value));
			} break;
			default:
				std::memset(at, 0, size_t(bytesPerSample));
				break;
			}
			at += bytesPerSample;
		}
	}
}

Resampler::Resampler(int fromRate, int toRate)
: _step(double(std::max(fromRate, 1)) / double(std::max(toRate, 1))) {
}

void Resampler::push(
		const qint16 *samples,
		int count,
		std::vector<qint16> &out) {
	if (count <= 0) {
		return;
	}
	if (!_primed) {
		_primed = true;
		_last = samples[0];
	}
	// _position is measured from the sample before samples[0] (_last).
	while (_position < double(count)) {
		const auto index = int(_position);
		const auto fraction = _position - index;
		const auto a = (index == 0) ? _last : samples[index - 1];
		const auto b = samples[index];
		out.push_back(Clip(a + (b - a) * fraction));
		_position += _step;
	}
	_position -= count;
	_last = samples[count - 1];
}

void PlayoutQueue::push(const qint16 *samples, int count) {
	if (count <= 0) {
		return;
	}
	if (_head > 0 && _head * 2 >= qsizetype(_samples.size())) {
		_samples.erase(_samples.begin(), _samples.begin() + _head);
		_head = 0;
	}
	_samples.insert(_samples.end(), samples, samples + count);
	if (queuedMs() > kMaxMs) {
		// The reader fell behind (a stalled device): skip ahead.
		const auto keep = qsizetype(kTrimToMs) * kSampleRate / 1000;
		_head = qsizetype(_samples.size()) - keep;
	}
}

void PlayoutQueue::pull(qint16 *samples, int count) {
	if (count <= 0) {
		return;
	}
	const auto available = qsizetype(_samples.size()) - _head;
	if (!_playing) {
		if (available * 1000 / kSampleRate < kStartMs) {
			std::fill(samples, samples + count, qint16(0));
			return;
		}
		_playing = true;
	}
	const auto take = int(std::min(qsizetype(count), available));
	std::copy(
		_samples.begin() + _head,
		_samples.begin() + _head + take,
		samples);
	_head += take;
	if (take < count) {
		// Ran dry: silence, and wait for a little queue again.
		std::fill(samples + take, samples + count, qint16(0));
		_playing = false;
	}
}

void PlayoutQueue::clear() {
	_samples.clear();
	_head = 0;
	_playing = false;
}

int PlayoutQueue::queuedMs() const {
	return int((qsizetype(_samples.size()) - _head) * 1000 / kSampleRate);
}

Input::Input(QObject *parent)
: QObject(parent) {
#ifdef ELEZA_DEBUG_TOOLS
	_generator.setTimerType(Qt::PreciseTimer);
	_generator.setInterval(kFrameMs);
	QObject::connect(&_generator, &QTimer::timeout, this, [this] { generate(); });
#endif // ELEZA_DEBUG_TOOLS
}

Input::~Input() {
	stop();
}

void Input::setFrameCallback(FrameCallback callback) {
	_frame = std::move(callback);
}

void Input::setFailedCallback(std::function<void()> callback) {
	_failed = std::move(callback);
}

bool Input::start(const QAudioDevice &device) {
	stop();
	if (device.isNull()) {
		return false;
	}
	const auto wanted = PreferredFormat(1);
	_format = device.isFormatSupported(wanted)
		? wanted
		: device.preferredFormat();
	if (!Usable(_format)) {
		return false;
	}
	_resampler = (_format.sampleRate() != kSampleRate)
		? std::make_unique<Resampler>(_format.sampleRate(), kSampleRate)
		: nullptr;
	_source = std::make_unique<QAudioSource>(device, _format);
	// Small chunks: what waits in the device's buffer is delay.
	_source->setBufferSize(_format.bytesForDuration(4 * kFrameMs * 1000));
	_io = _source->start();
	if (!_io || _source->error() != QAudio::NoError) {
		_io = nullptr;
		_source = nullptr;
		return false;
	}
	QObject::connect(_io, &QIODevice::readyRead, this, [this] { readDevice(); });
	QObject::connect(
		_source.get(),
		&QAudioSource::stateChanged,
		this,
		[this](QAudio::State state) {
			if (state == QAudio::StoppedState
				&& _active
				&& _source
				&& _source->error() != QAudio::NoError) {
				// Not from inside the device's own signal.
				QTimer::singleShot(0, this, [this] {
					if (_active && _source) {
						stop();
						if (_failed) {
							_failed();
						}
					}
				});
			}
		});
	_active = true;
	return true;
}

#ifdef ELEZA_DEBUG_TOOLS
bool Input::startGenerated(const QString &spec) {
	stop();
	const auto prefix = QStringLiteral("tone:");
	if (!spec.startsWith(prefix)) {
		return false;
	}
	_toneHz = spec.mid(prefix.size()).toDouble();
	if (_toneHz < 50. || _toneHz > 8000.) {
		return false;
	}
	_startedAtMs = QDateTime::currentMSecsSinceEpoch();
	_generated = 0;
	_clock.start();
	_generator.start();
	_active = true;
	return true;
}

void Input::generate() {
	if (!_active || !_frame) {
		return;
	}
	// As many frames as the clock says are due (a late timer catches up, a
	// stalled process does not flood the call afterwards).
	const auto due = _clock.elapsed() * kSampleRate / 1000;
	if (due - _generated > kSampleRate) {
		_generated = due - kFrameSamples;
	}
	qint16 frame[kFrameSamples];
	while (_generated + kFrameSamples <= due) {
		for (auto i = 0; i != kFrameSamples; ++i) {
			const auto index = _generated + i;
			const auto wallMs = _startedAtMs + index * 1000 / kSampleRate;
			const auto on = (wallMs % 1000) < 500;
			const auto phase = 2. * kPi * _toneHz * double(index) / kSampleRate;
			frame[i] = on ? qint16(std::sin(phase) * 9000.) : qint16(0);
		}
		_generated += kFrameSamples;
		_frame(frame);
		if (!_active) {
			return;
		}
	}
}
#endif // ELEZA_DEBUG_TOOLS

void Input::stop() {
	_active = false;
#ifdef ELEZA_DEBUG_TOOLS
	_generator.stop();
#endif // ELEZA_DEBUG_TOOLS
	if (_source) {
		_source->disconnect(this);
		if (_io) {
			_io->disconnect(this);
		}
		_source->stop();
		_source = nullptr;
	}
	_io = nullptr;
	_raw.clear();
	_mono.clear();
	_ready.clear();
	_resampler = nullptr;
}

int Input::delayMs() const {
	if (!_source || !_format.isValid()) {
		return kFrameMs;
	}
	const auto chunk = _format.durationForBytes(
		std::max(_source->bufferSize(), qsizetype(0))) / 1000;
	return int(std::clamp(chunk / 2 + kFrameMs, qint64(kFrameMs), qint64(120)));
}

void Input::readDevice() {
	if (!_io || !_active) {
		return;
	}
	_raw.append(_io->readAll());
	const auto bytesPerFrame = _format.bytesPerFrame();
	if (bytesPerFrame <= 0) {
		_raw.clear();
		return;
	}
	const auto whole = (_raw.size() / bytesPerFrame) * bytesPerFrame;
	if (whole <= 0) {
		return;
	}
	if (_resampler) {
		_mono.clear();
		ToMono16(_raw.constData(), whole, _format, _mono);
		_resampler->push(_mono.data(), int(_mono.size()), _ready);
	} else {
		ToMono16(_raw.constData(), whole, _format, _ready);
	}
	_raw.remove(0, whole);
	emitFrames();
}

void Input::emitFrames() {
	auto done = size_t();
	// A burst after a stall: keep the newest second, the rest is too old to send.
	if (_ready.size() > size_t(kSampleRate)) {
		done = ((_ready.size() - size_t(kSampleRate)) / kFrameSamples) * kFrameSamples;
	}
	while (_active && _ready.size() - done >= size_t(kFrameSamples)) {
		if (_frame) {
			_frame(_ready.data() + done);
		}
		done += kFrameSamples;
	}
	if (done > 0 && done <= _ready.size()) {
		_ready.erase(_ready.begin(), _ready.begin() + qsizetype(done));
	}
}

// What the sink reads from: sequential, always has data.
class Output::Device final : public QIODevice {
public:
	explicit Device(Output *output)
	: _output(output)
	, _resampler((output->_format.sampleRate() != kSampleRate)
		? std::make_unique<Resampler>(kSampleRate, output->_format.sampleRate())
		: nullptr) {
	}

	bool isSequential() const override {
		return true;
	}
	qint64 bytesAvailable() const override {
		return _output->_format.bytesForDuration(kFrameMs * 1000 * 4)
			+ QIODevice::bytesAvailable();
	}

protected:
	qint64 readData(char *data, qint64 maxSize) override {
		const auto &format = _output->_format;
		const auto bytesPerFrame = format.bytesPerFrame();
		if (bytesPerFrame <= 0 || maxSize < bytesPerFrame) {
			return 0;
		}
		const auto frames = int(std::min(
			maxSize / bytesPerFrame,
			qint64(kSampleRate))); // at most a second per read
		_converted.clear();
		if (!_resampler) {
			_mono.resize(size_t(frames));
			_output->consume(_mono.data(), frames);
			FromMono16(_mono.data(), frames, format, _converted);
		} else {
			// Ask the queue for what makes `frames` at the device's rate.
			while (int(_resampled.size()) < frames) {
				const auto need = frames - int(_resampled.size());
				const auto source = std::max(
					int(qint64(need) * kSampleRate / format.sampleRate()) + 1,
					kFrameSamples / 4);
				_mono.resize(size_t(source));
				_output->consume(_mono.data(), source);
				_resampler->push(_mono.data(), source, _resampled);
			}
			FromMono16(_resampled.data(), frames, format, _converted);
			_resampled.erase(_resampled.begin(), _resampled.begin() + frames);
		}
		std::memcpy(data, _converted.constData(), size_t(_converted.size()));
		return _converted.size();
	}
	qint64 writeData(const char *data, qint64 maxSize) override {
		Q_UNUSED(data);
		Q_UNUSED(maxSize);
		return -1;
	}

private:
	Output *_output = nullptr;
	std::unique_ptr<Resampler> _resampler;
	std::vector<qint16> _mono;
	std::vector<qint16> _resampled;
	QByteArray _converted;

};

Output::Output(QObject *parent)
: QObject(parent) {
#ifdef ELEZA_DEBUG_TOOLS
	_recorder.setTimerType(Qt::PreciseTimer);
	_recorder.setInterval(kFrameMs);
	QObject::connect(&_recorder, &QTimer::timeout, this, [this] { record(); });
#endif // ELEZA_DEBUG_TOOLS
}

Output::~Output() {
	stop();
}

bool Output::start(const QAudioDevice &device) {
	stop();
	if (device.isNull()) {
		return false;
	}
	const auto mono = PreferredFormat(1);
	const auto stereo = PreferredFormat(2);
	_format = device.isFormatSupported(mono)
		? mono
		: device.isFormatSupported(stereo)
		? stereo
		: device.preferredFormat();
	if (!Usable(_format)) {
		return false;
	}
	_device = std::make_unique<Device>(this);
	_device->open(QIODevice::ReadOnly | QIODevice::Unbuffered);
	_sink = std::make_unique<QAudioSink>(device, _format);
	_sink->setBufferSize(_format.bytesForDuration(4 * kFrameMs * 1000));
	_sink->start(_device.get());
	if (_sink->error() != QAudio::NoError) {
		_sink = nullptr;
		_device = nullptr;
		return false;
	}
	_active = true;
	return true;
}

#ifdef ELEZA_DEBUG_TOOLS
bool Output::startRecording(const QString &path) {
	stop();
	_file.setFileName(path);
	if (!_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	_file.write(WavHeader(0));
	auto sidecar = QFile(path + QStringLiteral(".json"));
	if (sidecar.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		sidecar.write(QJsonDocument(QJsonObject{
			{ QStringLiteral("started_ms"), double(QDateTime::currentMSecsSinceEpoch()) },
			{ QStringLiteral("rate"), kSampleRate },
		}).toJson(QJsonDocument::Compact));
	}
	_format = PreferredFormat(1);
	_recorded = 0;
	_clock.start();
	_recorder.start();
	_active = true;
	return true;
}

void Output::record() {
	if (!_file.isOpen()) {
		return;
	}
	const auto due = _clock.elapsed() * kSampleRate / 1000;
	qint16 frame[kFrameSamples];
	auto written = false;
	while (_recorded + kFrameSamples <= due) {
		consume(frame, kFrameSamples);
		_file.write(
			reinterpret_cast<const char*>(frame),
			qint64(sizeof(frame)));
		_recorded += kFrameSamples;
		written = true;
	}
	// A valid file at every second, also when the process is ended hard.
	if (written && (_recorded % kSampleRate) < kFrameSamples) {
		finishRecording();
	}
}

void Output::finishRecording() {
	if (!_file.isOpen()) {
		return;
	}
	const auto end = _file.pos();
	_file.seek(0);
	_file.write(WavHeader(quint32(_recorded * 2)));
	_file.seek(end);
	_file.flush();
}
#endif // ELEZA_DEBUG_TOOLS

void Output::stop() {
	_active = false;
#ifdef ELEZA_DEBUG_TOOLS
	_recorder.stop();
	if (_file.isOpen()) {
		finishRecording();
		_file.close();
	}
#endif // ELEZA_DEBUG_TOOLS
	if (_sink) {
		_sink->stop();
		_sink = nullptr;
	}
	_device = nullptr;
	const auto lock = QMutexLocker(&_mutex);
	_queue.clear();
	_reference.clear();
	_level = 0.;
}

void Output::push(const qint16 *samples, int count) {
	const auto lock = QMutexLocker(&_mutex);
	if (_active) {
		_queue.push(samples, count);
	}
}

void Output::consume(qint16 *samples, int count) {
	const auto lock = QMutexLocker(&_mutex);
	_queue.pull(samples, count);
	_reference.insert(_reference.end(), samples, samples + count);
	const auto limit = size_t(kMaxReferenceMs) * kSampleRate / 1000;
	if (_reference.size() > limit) {
		// Nobody takes the reference (the microphone is closed).
		_reference.erase(
			_reference.begin(),
			_reference.begin() + qsizetype(_reference.size() - limit));
	}
	_level = std::max(_level, Level(samples, count));
}

void Output::takeReference(std::vector<qint16> &out) {
	const auto lock = QMutexLocker(&_mutex);
	out.insert(out.end(), _reference.begin(), _reference.end());
	_reference.clear();
}

float Output::takeLevel() {
	const auto lock = QMutexLocker(&_mutex);
	return std::exchange(_level, 0.f);
}

int Output::delayMs() const {
	if (!_sink || !_format.isValid()) {
		return 0;
	}
	const auto waiting = std::max(
		_sink->bufferSize() - _sink->bytesFree(),
		qsizetype(0));
	return int(std::clamp(
		_format.durationForBytes(qint32(waiting)) / 1000,
		qint64(0),
		qint64(300)));
}

} // namespace Calls::Audio
