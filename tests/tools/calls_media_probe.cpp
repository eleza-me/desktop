// A participant of a LiveKit room without the app around it: the media layer
// of voice calls (Eleza/SourceFiles/calls/calls_media*) driven from the
// command line. Used by tests/tools/calls-e2e.sh and handy against a real SFU.
//
//   calls_media_probe --url=ws://127.0.0.1:7880 --token=<jwt>
//       [--input=tone:440]      a generated tone instead of the microphone
//       [--output=<file.wav>]   record what would be played
//       [--seconds=10]          leave after that long (default: 10)
//       [--processing=aec,ns,agc | off]   what the processing module does (default: all)
//       [--mute-at=<s>] [--unmute-at=<s>]
//       [--rejoin-at=<s> --rejoin-token=<jwt>]   join again with another token
//       [--camera=pattern]      publish generated video frames as the camera
//       [--camera-off-at=<s>] [--camera-on-at=<s>]
//
// Prints one line per event:
//
//   state connecting | connected | reconnecting | failed | idle
//   levels <input> <output>      (once a second)
//   stats rtt_ms=… jitter_ms=… loss_pct=… mos=… relay_tcp=…
//   camera on | off | failed
//   video local=<frames sent this second> remote=<frames received> size=<WxH> shown=<0|1>
//   remote-video on | off
//
// Exit code 0 when the room was joined at least once.
//
//   calls_media_probe --self-check
//
// loads the library that lies next to this program, starts it and asks it
// for an audio processing module and for a video source: the interface is
// what this build expects (no network, no audio device, no camera). Exit code 0 = it answered, 77 = there is no
// library for this platform (the test is skipped), 1 = it is there and does
// not work. Runs as the test `test_calls_media_library`.
#include "calls/calls_ffi.h"
#include "calls/calls_media.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>

#include <cstdio>

namespace {

[[nodiscard]] QString Argument(const QStringList &arguments, const QString &name) {
	const auto prefix = QStringLiteral("--") + name + QLatin1Char('=');
	for (const auto &argument : arguments) {
		if (argument.startsWith(prefix)) {
			return argument.mid(prefix.size());
		}
	}
	return QString();
}

void Print(const QString &line) {
	std::fprintf(stdout, "%s\n", qPrintable(line));
	std::fflush(stdout);
}

[[nodiscard]] QString StateName(Calls::Media::State state) {
	switch (state) {
	case Calls::Media::State::Idle: return QStringLiteral("idle");
	case Calls::Media::State::Connecting: return QStringLiteral("connecting");
	case Calls::Media::State::Connected: return QStringLiteral("connected");
	case Calls::Media::State::Reconnecting: return QStringLiteral("reconnecting");
	case Calls::Media::State::Failed: return QStringLiteral("failed");
	}
	return QStringLiteral("unknown");
}

[[nodiscard]] int SelfCheck() {
	using namespace Calls::Ffi;

	if (!Library::Present()) {
		std::fprintf(stderr, "no %s next to this program: skipped\n", qPrintable(Library::FileName()));
		return 77;
	}
	const auto library = Library::Instance();
	if (!library) {
		std::fprintf(stderr, "the library does not load: %s\n", qPrintable(Library::LoadError()));
		return 1;
	}
	// FfiRequest { new_apm = 50 { echo_canceller = 1, gain_controller = 2,
	// high_pass_filter = 3, noise_suppression = 4 } } ->
	// FfiResponse { new_apm = 49 { apm = 1 { handle = 1 { id = 1 } } } }.
	auto message = Writer();
	message.boolean(1, true).boolean(2, true).boolean(3, true).boolean(4, true);
	const auto response = library->request(50, message);
	auto handle = quint64(0);
	auto reader = Reader(response);
	while (reader.next()) {
		if (reader.field() != 49) {
			continue;
		}
		auto answer = reader.sub();
		while (answer.next()) {
			if (answer.field() != 1) {
				continue;
			}
			auto owned = answer.sub();
			while (owned.next()) {
				if (owned.field() != 1) {
					continue;
				}
				auto inner = owned.sub();
				while (inner.next()) {
					if (inner.field() == 1) {
						handle = inner.varint();
					}
				}
			}
		}
	}
	if (!reader.ok() || !handle) {
		std::fprintf(stderr, "the library answered something else (%d bytes)\n", int(response.size()));
		return 1;
	}
	library->dropHandle(handle);
	// FfiRequest { new_video_source = 21 { type = 1, resolution = 2 { width = 1,
	// height = 2 }, is_screencast = 3 } } ->
	// FfiResponse { new_video_source = 21 { source = 1 { handle = 1 { id = 1 } } } }:
	// what a camera of a video call is published through.
	auto resolution = Writer();
	resolution.varint(1, 640).varint(2, 480);
	auto video = Writer();
	video.varint(1, 0).message(2, resolution).boolean(3, false);
	const auto videoResponse = library->request(21, video);
	auto videoHandle = quint64(0);
	auto videoReader = Reader(videoResponse);
	while (videoReader.next()) {
		if (videoReader.field() != 21) {
			continue;
		}
		auto answer = videoReader.sub();
		while (answer.next()) {
			if (answer.field() != 1) {
				continue;
			}
			auto owned = answer.sub();
			while (owned.next()) {
				if (owned.field() != 1) {
					continue;
				}
				auto inner = owned.sub();
				while (inner.next()) {
					if (inner.field() == 1) {
						videoHandle = inner.varint();
					}
				}
			}
		}
	}
	if (!videoReader.ok() || !videoHandle) {
		std::fprintf(stderr, "the library gave no video source (%d bytes)\n", int(videoResponse.size()));
		return 1;
	}
	library->dropHandle(videoHandle);
	library->dispose();
	Print(QStringLiteral("%1: loaded; a processing module and a video source were created and released").arg(Library::FileName()));
	return 0;
}

} // namespace

int main(int argc, char *argv[]) {
	// The generated camera paints text: that takes the GUI application (on
	// the offscreen platform when nothing else is asked for). Everything
	// else runs without one.
	auto withCamera = false;
	for (auto i = 1; i < argc; ++i) {
		withCamera = withCamera
			|| QByteArray(argv[i]).startsWith("--camera=");
	}
	if (withCamera && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
		qputenv("QT_QPA_PLATFORM", "offscreen");
	}
	const auto owned = withCamera
		? std::unique_ptr<QCoreApplication>(new QGuiApplication(argc, argv))
		: std::make_unique<QCoreApplication>(argc, argv);
	auto &application = *owned;
	const auto arguments = application.arguments();
	if (arguments.contains(QStringLiteral("--self-check"))) {
		return SelfCheck();
	}
	auto join = Calls::MediaJoin();
	join.url = Argument(arguments, QStringLiteral("url"));
	join.token = Argument(arguments, QStringLiteral("token"));
	if (!join.valid()) {
		std::fprintf(stderr, "usage: calls_media_probe --url=<ws url> --token=<jwt> [--input=tone:<Hz>] [--output=<wav>] [--seconds=<n>]\n");
		return 2;
	}
	if (Calls::CheckMedia() != Calls::MediaProblem::None) {
		std::fprintf(stderr, "the media library is not next to this program\n");
		return 3;
	}
	auto settings = Calls::MediaSettings();
#ifdef ELEZA_DEBUG_TOOLS
	settings.debugInput = Argument(arguments, QStringLiteral("input"));
	settings.debugOutput = Argument(arguments, QStringLiteral("output"));
	settings.debugCamera = Argument(arguments, QStringLiteral("camera"));
#endif // ELEZA_DEBUG_TOOLS
	// "--processing=aec,ns,agc" (any of them), "off"; all three by default.
	const auto processing = Argument(arguments, QStringLiteral("processing"));
	if (!processing.isEmpty()) {
		const auto parts = processing.split(QLatin1Char(','));
		settings.echoCancellation = parts.contains(QStringLiteral("aec"));
		settings.noiseSuppression = parts.contains(QStringLiteral("ns"));
		settings.autoGainControl = parts.contains(QStringLiteral("agc"));
	}
	auto media = Calls::CreateMedia(settings);
	if (!media) {
		std::fprintf(stderr, "the media library could not be loaded\n");
		return 3;
	}
	auto joined = false;
	QObject::connect(media.get(), &Calls::Media::stateChanged, [&](
			Calls::Media::State state) {
		Print(QStringLiteral("state ") + StateName(state));
		if (state == Calls::Media::State::Connected) {
			joined = true;
		}
	});
	auto input = 0.f;
	auto output = 0.f;
	QObject::connect(media.get(), &Calls::Media::levelsChanged, [&](
			float nowInput,
			float nowOutput) {
		input = std::max(input, nowInput);
		output = std::max(output, nowOutput);
	});
	QObject::connect(media.get(), &Calls::Media::statsReady, [&](
			const Calls::MediaStats &stats) {
		Print(stats.valid
			? QStringLiteral("stats rtt_ms=%1 jitter_ms=%2 loss_pct=%3 mos=%4 relay_tcp=%5")
				.arg(stats.rttMs, 0, 'f', 1)
				.arg(stats.jitterMs, 0, 'f', 1)
				.arg(stats.lossPercent, 0, 'f', 2)
				.arg(stats.mos, 0, 'f', 2)
				.arg(stats.relayTcp ? 1 : 0)
			: QStringLiteral("stats none"));
	});
	QObject::connect(media.get(), &Calls::Media::inputFailed, [] {
		Print(QStringLiteral("input failed"));
	});
	// ---- video
	auto localFrames = 0;
	auto remoteFrames = 0;
	auto remoteSize = QSize();
	auto remoteShown = false;
	QObject::connect(media.get(), &Calls::Media::cameraChanged, [](bool on) {
		Print(on ? QStringLiteral("camera on") : QStringLiteral("camera off"));
	});
	QObject::connect(media.get(), &Calls::Media::cameraFailed, [](bool) {
		Print(QStringLiteral("camera failed"));
	});
	QObject::connect(media.get(), &Calls::Media::localVideoFrame, [&](const QImage &) {
		++localFrames;
	});
	QObject::connect(media.get(), &Calls::Media::remoteVideoFrame, [&](const QImage &frame) {
		++remoteFrames;
		remoteSize = frame.size();
	});
	QObject::connect(media.get(), &Calls::Media::remoteVideoChanged, [&](bool shown) {
		remoteShown = shown;
		Print(shown ? QStringLiteral("remote-video on") : QStringLiteral("remote-video off"));
	});
	const auto cameraOffAt = Argument(arguments, QStringLiteral("camera-off-at")).toInt();
	const auto cameraOnAt = Argument(arguments, QStringLiteral("camera-on-at")).toInt();
	if (withCamera) {
		media->setCameraEnabled(true);
	}
	auto second = QTimer();
	auto elapsed = 0;
	const auto seconds = std::max(Argument(arguments, QStringLiteral("seconds")).toInt(), 0);
	const auto limit = seconds ? seconds : 10;
	const auto muteAt = Argument(arguments, QStringLiteral("mute-at")).toInt();
	const auto unmuteAt = Argument(arguments, QStringLiteral("unmute-at")).toInt();
	const auto rejoinAt = Argument(arguments, QStringLiteral("rejoin-at")).toInt();
	const auto rejoinToken = Argument(arguments, QStringLiteral("rejoin-token"));
	QObject::connect(&second, &QTimer::timeout, [&] {
		++elapsed;
		Print(QStringLiteral("levels %1 %2")
			.arg(std::exchange(input, 0.f), 0, 'f', 2)
			.arg(std::exchange(output, 0.f), 0, 'f', 2));
		if (withCamera || remoteFrames || remoteShown) {
			Print(QStringLiteral("video local=%1 remote=%2 size=%3x%4 shown=%5")
				.arg(std::exchange(localFrames, 0))
				.arg(std::exchange(remoteFrames, 0))
				.arg(remoteSize.width())
				.arg(remoteSize.height())
				.arg(remoteShown ? 1 : 0));
		}
		if (cameraOffAt && elapsed == cameraOffAt) {
			media->setCameraEnabled(false);
		}
		if (cameraOnAt && elapsed == cameraOnAt) {
			media->setCameraEnabled(true);
		}
		if (muteAt && elapsed == muteAt) {
			Print(QStringLiteral("mute"));
			media->setMuted(true);
		}
		if (unmuteAt && elapsed == unmuteAt) {
			Print(QStringLiteral("unmute"));
			media->setMuted(false);
		}
		if (rejoinAt && elapsed == rejoinAt && !rejoinToken.isEmpty()) {
			Print(QStringLiteral("rejoin"));
			auto again = join;
			again.token = rejoinToken;
			media->join(again);
		}
		if (elapsed == limit - 1) {
			media->requestStats();
		}
		if (elapsed >= limit) {
			media->leave();
			QTimer::singleShot(300, &application, [&] { application.quit(); });
		}
	});
	second.start(1000);
	media->join(join);
	const auto result = application.exec();
	media = nullptr;
	Calls::ShutdownMedia();
	return (result == 0 && joined) ? 0 : 1;
}
