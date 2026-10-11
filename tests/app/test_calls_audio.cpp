// The pure parts of the call audio (calls/calls_audio_io.h, calls_media.h):
// sample formats, the fallback resampler, the queue in front of the
// loudspeaker, the level meter, the join payload and the quality estimate.
#include "calls/calls_audio_io.h"
#include "calls/calls_media.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtTest/QtTest>

#include <cmath>
#include <cstring>

using namespace Calls;
using namespace Calls::Audio;

namespace {

constexpr auto kPi = 3.14159265358979323846;

[[nodiscard]] QAudioFormat Format(
		int rate,
		int channels,
		QAudioFormat::SampleFormat sample) {
	auto result = QAudioFormat();
	result.setSampleRate(rate);
	result.setChannelCount(channels);
	result.setSampleFormat(sample);
	return result;
}

[[nodiscard]] std::vector<qint16> Sine(int count, double hz, double amplitude) {
	auto result = std::vector<qint16>(size_t(count));
	for (auto i = 0; i != count; ++i) {
		result[size_t(i)] = qint16(std::lround(
			amplitude * 32767. * std::sin(2. * kPi * hz * i / kSampleRate)));
	}
	return result;
}

} // namespace

class TestCallsAudio final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void formats_data();
	void formats();
	void stereoIsMixedDown();
	void oddBytesAreIgnored();
	void resamplerKeepsLengthAndTone_data();
	void resamplerKeepsLengthAndTone();
	void resamplerIsContinuousAcrossPushes();
	void queueWaitsThenPlaysInOrder();
	void queueRunsDryAndStartsAgain();
	void queueDropsOldAudioWhenTheReaderStalls();
	void level();
	void pickDevice();
	void joinPayload();
	void joinPayloadWithoutTurn();
	void mos();

};

void TestCallsAudio::formats_data() {
	QTest::addColumn<int>("sample");
	QTest::addColumn<int>("tolerance");

	QTest::newRow("16-bit") << int(QAudioFormat::Int16) << 0;
	QTest::newRow("32-bit") << int(QAudioFormat::Int32) << 0;
	QTest::newRow("float") << int(QAudioFormat::Float) << 1;
	QTest::newRow("8-bit") << int(QAudioFormat::UInt8) << 256;
}

void TestCallsAudio::formats() {
	QFETCH(int, sample);
	QFETCH(int, tolerance);

	// There and back through the device's format, mono and stereo.
	const qint16 source[] = { 0, 1, -1, 1000, -1000, 32767, -32768, 12345 };
	const auto count = int(std::size(source));
	for (const auto channels : { 1, 2 }) {
		const auto format = Format(
			kSampleRate,
			channels,
			QAudioFormat::SampleFormat(sample));
		auto bytes = QByteArray("x");
		FromMono16(source, count, format, bytes);
		QCOMPARE(bytes.size(), 1 + qsizetype(count) * format.bytesPerFrame());
		QCOMPARE(bytes.at(0), 'x'); // appended, nothing overwritten

		auto back = std::vector<qint16>{ qint16(7) };
		ToMono16(bytes.constData() + 1, bytes.size() - 1, format, back);
		QCOMPARE(int(back.size()), 1 + count);
		QCOMPARE(back[0], qint16(7));
		for (auto i = 0; i != count; ++i) {
			const auto difference = std::abs(int(back[size_t(i) + 1]) - int(source[i]));
			QVERIFY2(
				difference <= tolerance,
				qPrintable(QStringLiteral("sample %1: %2 -> %3")
					.arg(i)
					.arg(source[i])
					.arg(back[size_t(i) + 1])));
		}
	}
}

void TestCallsAudio::stereoIsMixedDown() {
	const qint16 frames[] = { 1000, 3000, -2000, 2000, 32767, 32767 };
	auto out = std::vector<qint16>();
	ToMono16(
		reinterpret_cast<const char*>(frames),
		sizeof(frames),
		Format(kSampleRate, 2, QAudioFormat::Int16),
		out);
	QCOMPARE(int(out.size()), 3);
	QCOMPARE(out[0], qint16(2000));
	QCOMPARE(out[1], qint16(0));
	QCOMPARE(out[2], qint16(32767));
}

void TestCallsAudio::oddBytesAreIgnored() {
	// Half a frame at the end of what the device gave stays unread.
	const qint16 frames[] = { 10, 20, 30 };
	auto out = std::vector<qint16>();
	ToMono16(
		reinterpret_cast<const char*>(frames),
		sizeof(frames),
		Format(kSampleRate, 2, QAudioFormat::Int16),
		out);
	QCOMPARE(int(out.size()), 1);
	QCOMPARE(out[0], qint16(15));
	// A format nobody can describe gives nothing instead of garbage.
	ToMono16(reinterpret_cast<const char*>(frames), sizeof(frames), QAudioFormat(), out);
	QCOMPARE(int(out.size()), 1);
	auto bytes = QByteArray();
	FromMono16(frames, 3, QAudioFormat(), bytes);
	QVERIFY(bytes.isEmpty());
}

void TestCallsAudio::resamplerKeepsLengthAndTone_data() {
	QTest::addColumn<int>("from");
	QTest::addColumn<int>("to");

	QTest::newRow("44.1 -> 48") << 44100 << 48000;
	QTest::newRow("48 -> 44.1") << 48000 << 44100;
	QTest::newRow("16 -> 48") << 16000 << 48000;
	QTest::newRow("48 -> 16") << 48000 << 16000;
	QTest::newRow("48 -> 48") << 48000 << 48000;
}

void TestCallsAudio::resamplerKeepsLengthAndTone() {
	QFETCH(int, from);
	QFETCH(int, to);

	// One second of a 440 Hz tone at `from`.
	auto source = std::vector<qint16>(size_t(from));
	for (auto i = 0; i != from; ++i) {
		source[size_t(i)] = qint16(std::lround(
			16000. * std::sin(2. * kPi * 440. * i / from)));
	}
	auto resampler = Resampler(from, to);
	auto out = std::vector<qint16>();
	// In pieces of 10 ms, as a device delivers it.
	const auto piece = from / 100;
	for (auto at = 0; at < from; at += piece) {
		resampler.push(source.data() + at, std::min(piece, from - at), out);
	}
	QVERIFY2(
		std::abs(int(out.size()) - to) <= 2,
		qPrintable(QStringLiteral("%1 samples").arg(out.size())));

	// Still the same tone: it crosses zero upwards 440 times a second, and
	// is as loud as it was.
	auto crossings = 0;
	auto peak = 0;
	for (auto i = size_t(1); i < out.size(); ++i) {
		if (out[i - 1] < 0 && out[i] >= 0) {
			++crossings;
		}
		peak = std::max(peak, std::abs(int(out[i])));
	}
	QVERIFY2(std::abs(crossings - 440) <= 1, qPrintable(QString::number(crossings)));
	QVERIFY2(peak > 15000 && peak <= 16000, qPrintable(QString::number(peak)));
}

void TestCallsAudio::resamplerIsContinuousAcrossPushes() {
	// No click where two pieces meet: the output never jumps further than
	// the tone itself moves between two samples.
	const auto source = Sine(4800, 300., 0.5);
	auto resampler = Resampler(48000, 44100);
	auto out = std::vector<qint16>();
	for (auto at = 0; at < 4800; at += 7) {
		resampler.push(source.data() + at, std::min(7, 4800 - at), out);
	}
	auto largest = 0;
	for (auto i = size_t(1); i < out.size(); ++i) {
		largest = std::max(largest, std::abs(int(out[i]) - int(out[i - 1])));
	}
	// 300 Hz at half scale moves at most ~700 units per sample at 44.1 kHz.
	QVERIFY2(largest < 800, qPrintable(QString::number(largest)));
	// Nothing in, nothing out.
	const auto before = out.size();
	resampler.push(source.data(), 0, out);
	QCOMPARE(out.size(), before);
}

void TestCallsAudio::queueWaitsThenPlaysInOrder() {
	auto queue = PlayoutQueue();
	auto frame = std::vector<qint16>(kFrameSamples);
	auto played = std::vector<qint16>(kFrameSamples, qint16(99));

	// Nothing queued: silence.
	queue.pull(played.data(), kFrameSamples);
	QVERIFY(std::all_of(played.begin(), played.end(), [](qint16 v) { return v == 0; }));

	// Two frames (20 ms) are not enough to start.
	for (auto i = 0; i != 2; ++i) {
		std::fill(frame.begin(), frame.end(), qint16(i + 1));
		queue.push(frame.data(), kFrameSamples);
	}
	QCOMPARE(queue.queuedMs(), 20);
	queue.pull(played.data(), kFrameSamples);
	QCOMPARE(played[0], qint16(0));
	QCOMPARE(queue.queuedMs(), 20);

	// The third one starts it; frames come out in the order they went in.
	std::fill(frame.begin(), frame.end(), qint16(3));
	queue.push(frame.data(), kFrameSamples);
	for (auto i = 0; i != 3; ++i) {
		queue.pull(played.data(), kFrameSamples);
		QCOMPARE(played[0], qint16(i + 1));
		QCOMPARE(played[kFrameSamples - 1], qint16(i + 1));
	}
	QCOMPARE(queue.queuedMs(), 0);
}

void TestCallsAudio::queueRunsDryAndStartsAgain() {
	auto queue = PlayoutQueue();
	auto frame = std::vector<qint16>(kFrameSamples, qint16(5));
	for (auto i = 0; i != 3; ++i) {
		queue.push(frame.data(), kFrameSamples);
	}
	// The device asks for more than there is: the rest is silence.
	auto played = std::vector<qint16>(kFrameSamples * 4, qint16(99));
	queue.pull(played.data(), int(played.size()));
	QCOMPARE(played[0], qint16(5));
	QCOMPARE(played[kFrameSamples * 3 - 1], qint16(5));
	QCOMPARE(played[kFrameSamples * 3], qint16(0));
	QCOMPARE(played.back(), qint16(0));

	// After running dry it waits for a little queue again.
	queue.push(frame.data(), kFrameSamples);
	queue.pull(played.data(), kFrameSamples);
	QCOMPARE(played[0], qint16(0));
	queue.push(frame.data(), kFrameSamples);
	queue.push(frame.data(), kFrameSamples);
	queue.pull(played.data(), kFrameSamples);
	QCOMPARE(played[0], qint16(5));

	queue.clear();
	QCOMPARE(queue.queuedMs(), 0);
	queue.pull(played.data(), kFrameSamples);
	QCOMPARE(played[0], qint16(0));
	// Asking for nothing is harmless.
	queue.pull(played.data(), 0);
	queue.push(frame.data(), 0);
	QCOMPARE(queue.queuedMs(), 0);
}

void TestCallsAudio::queueDropsOldAudioWhenTheReaderStalls() {
	auto queue = PlayoutQueue();
	auto frame = std::vector<qint16>(kFrameSamples);
	// One second arrives while nothing is taken: latency must not pile up.
	for (auto i = 0; i != 100; ++i) {
		std::fill(frame.begin(), frame.end(), qint16(i));
		queue.push(frame.data(), kFrameSamples);
		QVERIFY(queue.queuedMs() <= PlayoutQueue::kMaxMs);
	}
	QVERIFY(queue.queuedMs() >= PlayoutQueue::kTrimToMs);
	// What plays next is recent, and the newest frame is still the last one.
	auto played = std::vector<qint16>(kFrameSamples);
	queue.pull(played.data(), kFrameSamples);
	QVERIFY2(played[0] >= 100 - PlayoutQueue::kMaxMs / kFrameMs, qPrintable(QString::number(played[0])));
	auto last = played[0];
	while (queue.queuedMs() > 0) {
		queue.pull(played.data(), kFrameSamples);
		QVERIFY(played[0] > last);
		last = played[0];
	}
	QCOMPARE(last, qint16(99));
}

void TestCallsAudio::level() {
	QCOMPARE(Level(nullptr, 0), 0.f);
	const auto silence = std::vector<qint16>(kFrameSamples, qint16(0));
	QCOMPARE(Level(silence.data(), kFrameSamples), 0.f);
	// Below -60 dB counts as silence.
	const auto hiss = std::vector<qint16>(kFrameSamples, qint16(20));
	QCOMPARE(Level(hiss.data(), kFrameSamples), 0.f);

	const auto full = std::vector<qint16>(kFrameSamples, qint16(32767));
	QVERIFY(Level(full.data(), kFrameSamples) > 0.99f);
	// A decibel scale: -20 dB is two thirds of the meter, -40 dB one third.
	const auto loud = Sine(kSampleRate / 10, 1000., 0.1 * std::sqrt(2.));
	QVERIFY(std::abs(Level(loud.data(), int(loud.size())) - 2.f / 3.f) < 0.01f);
	const auto quiet = Sine(kSampleRate / 10, 1000., 0.01 * std::sqrt(2.));
	QVERIFY(std::abs(Level(quiet.data(), int(quiet.size())) - 1.f / 3.f) < 0.02f);
}

void TestCallsAudio::pickDevice() {
	// No devices on the test machine: the rules still hold.
	const auto none = QList<QAudioDevice>();
	auto found = false;
	QVERIFY(PickDevice(none, QAudioDevice(), QByteArray(), &found).isNull());
	QVERIFY(found); // "the default" is always what was asked for
	QVERIFY(PickDevice(none, QAudioDevice(), QByteArray("usb-headset"), &found).isNull());
	QVERIFY(!found); // the chosen one is gone: the caller tells the user
	QVERIFY(PickDevice(none, QAudioDevice(), QByteArray("usb-headset")).isNull());
}

void TestCallsAudio::joinPayload() {
	const auto join = MediaJoin::FromJson(QJsonObject{
		{ QStringLiteral("call"), QStringLiteral("ca1") },
		{ QStringLiteral("state"), QStringLiteral("ringing") },
		{ QStringLiteral("sfu_url"), QStringLiteral("wss://sfu.example") },
		{ QStringLiteral("token"), QStringLiteral("jwt") },
		{ QStringLiteral("token_expires_at"), 1791675000 },
		{ QStringLiteral("turn"), QJsonObject{
			{ QStringLiteral("username"), QStringLiteral("1791675000:ca1") },
			{ QStringLiteral("credential"), QStringLiteral("secret") },
			{ QStringLiteral("urls"), QJsonArray{
				QStringLiteral("turn:turn.example:3478?transport=udp"),
				QStringLiteral("turns:turn.example:5349?transport=tcp"),
				QStringLiteral("https://not-an-ice-server.example"),
				QStringLiteral(""),
				42,
			} },
		} },
		{ QStringLiteral("peer"), QStringLiteral("u_sara") },
		{ QStringLiteral("relay_only"), true },
	});
	QVERIFY(join.valid());
	QCOMPARE(join.url, QStringLiteral("wss://sfu.example"));
	QCOMPARE(join.token, QStringLiteral("jwt"));
	QCOMPARE(join.turnUrls, (QStringList{
		QStringLiteral("turn:turn.example:3478?transport=udp"),
		QStringLiteral("turns:turn.example:5349?transport=tcp"),
	}));
	QCOMPARE(join.turnUsername, QStringLiteral("1791675000:ca1"));
	QCOMPARE(join.turnCredential, QStringLiteral("secret"));
	QVERIFY(join.relayOnly);
}

void TestCallsAudio::joinPayloadWithoutTurn() {
	const auto join = MediaJoin::FromJson(QJsonObject{
		{ QStringLiteral("sfu_url"), QStringLiteral("ws://127.0.0.1:7880") },
		{ QStringLiteral("token"), QStringLiteral("jwt") },
		{ QStringLiteral("turn"), QJsonObject{
			{ QStringLiteral("username"), QStringLiteral("user") },
			{ QStringLiteral("credential"), QStringLiteral("secret") },
			{ QStringLiteral("urls"), QJsonArray() },
		} },
		{ QStringLiteral("relay_only"), false },
	});
	QVERIFY(join.valid());
	QVERIFY(join.turnUrls.isEmpty());
	// Credentials without a server are of no use to anybody.
	QVERIFY(join.turnUsername.isEmpty());
	QVERIFY(join.turnCredential.isEmpty());
	QVERIFY(!join.relayOnly);

	// An older server does not say: relayed, as every call is.
	QVERIFY(MediaJoin::FromJson(QJsonObject{
		{ QStringLiteral("sfu_url"), QStringLiteral("wss://sfu.example") },
		{ QStringLiteral("token"), QStringLiteral("jwt") },
	}).relayOnly);
	QVERIFY(!MediaJoin::FromJson(QJsonObject()).valid());
	QVERIFY(!MediaJoin::FromJson(QJsonObject{
		{ QStringLiteral("sfu_url"), QStringLiteral("wss://sfu.example") },
	}).valid());
}

void TestCallsAudio::mos() {
	// A good connection is near the top of the scale, and the scale has
	// ends.
	const auto good = EstimateMos(20., 2., 0.);
	QVERIFY2(good > 4.3 && good <= 4.5, qPrintable(QString::number(good)));
	QCOMPARE(EstimateMos(5000., 500., 100.), 1.);
	QVERIFY(EstimateMos(-5., -5., -5.) <= 4.5);
	// Worse numbers never give a better grade.
	auto last = 5.;
	for (const auto loss : { 0., 1., 3., 5., 10., 20., 40. }) {
		const auto value = EstimateMos(80., 10., loss);
		QVERIFY(value <= last);
		last = value;
	}
	last = 5.;
	for (const auto rtt : { 10., 100., 300., 600., 1200. }) {
		const auto value = EstimateMos(rtt, 5., 0.5);
		QVERIFY(value <= last);
		last = value;
	}
	// The line the quality report draws: 5% loss is clearly worse than none.
	QVERIFY(EstimateMos(60., 5., 0.) - EstimateMos(60., 5., 5.) > 0.3);
}

QTEST_APPLESS_MAIN(TestCallsAudio)
#include "test_calls_audio.moc"
