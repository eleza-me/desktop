#include "media/media_module.h"

#include "core/core_application.h"
#include "media/media_player_widgets.h"
#include "media/media_shared.h"
#include "window/window_main_window.h"

#ifdef ELEZA_DEBUG_TOOLS
#include "lang/lang_format.h"
#include "main/main_session.h"
#endif // ELEZA_DEBUG_TOOLS
#include "window/window_registry.h"
#include "window/window_section_widget.h"
#include "window/window_session_controller.h"

namespace Media {

#ifdef ELEZA_DEBUG_TOOLS
// "debug/player[/voice|/box]": the bar with made-up songs of the chat
// `gallery` (or a voice), «box» opens the music player too.
bool HandleDebugRoute(
		Window::SessionController *controller,
		const QString &route) {
	const auto prefix = QStringLiteral("debug/player");
	if (route != prefix && !route.startsWith(prefix + QLatin1Char('/'))) {
		return false;
	}
	const auto mode = route.mid(prefix.size() + 1);
	auto queue = QVector<AudioTrack>();
	for (auto i = 0; i != 12; ++i) {
		auto track = AudioTrack();
		track.chat = QStringLiteral("gallery");
		track.seq = 200 - i * 5;
		track.id = AudioTrack::IdForMessage(track.chat, track.seq);
		track.ref = Data::MediaRef::Attachment(track.chat, track.seq);
		track.title = QStringLiteral("Track %1").arg(i + 1);
		track.performer = (i % 3) ? QStringLiteral("Homayoun Shajarian") : QStringLiteral("Hayedeh");
		track.durationMs = (180 + i * 7) * 1000;
		track.sender = controller->session().userId();
		queue.push_back(track);
	}
	if (mode == QStringLiteral("voice")) {
		auto voice = queue[1];
		voice.voice = true;
		voice.title = QString();
		voice.performer = QString();
		Player().debugShow(voice, {}, &controller->session());
	} else {
		Player().debugShow(queue[2], queue, &controller->session());
	}
	if (mode == QStringLiteral("box")) {
		ShowMusicPlayer(controller);
	}
	return true;
}
#endif // ELEZA_DEBUG_TOOLS

void Register() {
#ifdef ELEZA_DEBUG_TOOLS
	Window::RegisterRouteHandler(HandleDebugRoute);
#endif // ELEZA_DEBUG_TOOLS
	// chat/<id>?show=media; any other chat route goes on to the chat screen
	// (this handler is registered before it).
	Window::RegisterRouteHandler(Window::RouteType::Chat, [](
			Window::SessionController *controller,
			const Window::Route &route) {
		if (route.id.isEmpty()
			|| route.queryValue(QStringLiteral("show")) != QStringLiteral("media")) {
			return false;
		}
		// Over whatever opened it: the chat, the profile.
		controller->showMainSection(SharedMediaFactory(route.id));
		return true;
	});
	// The «now playing» bar of every session layout (a new one after the
	// language switch rebuilds the window).
	auto &app = Core::App();
	QObject::connect(
		&app,
		&Core::Application::sessionShownChanged,
		&app,
		[] {
			const auto window = Core::App().window();
			const auto controller = window ? window->sessionController() : nullptr;
			if (controller && !controller->property("elezaNowPlaying").toBool()) {
				controller->setProperty("elezaNowPlaying", true);
				SetupNowPlaying(controller);
			}
		});
}

} // namespace Media
