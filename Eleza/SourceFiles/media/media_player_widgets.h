// What the user sees of the app's audio player (media_audio_player.h):
//
// * the «now playing» bar on top of the main column (Telegram's): play /
//   pause, ⏮ ⏭ through the queue, the title — performer (the sender — voice
//   message), the voice speed, the playlist, close, a seekable progress line.
//   A song's title opens the music player, a voice's goes to its message;
// * the music player (a box): the cover, the time, repeat (off → all → one),
//   shuffle, reverse order, «نمایش در چت», and the playlist with the song
//   playing marked, paged in from the chat's media as it is scrolled.
//
//   Media::SetupNowPlaying(controller);   // once per session layout (Media::Register does it)
//   Media::OpenTrackPlace(controller, Media::Player().current());
#pragma once

#include "media/media_audio_player.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Media {

// The bar for the session layout of `controller`, and the toasts of the
// player's failures there.
void SetupNowPlaying(Window::SessionController *controller);

void ShowMusicPlayer(Window::SessionController *controller);

// Where a track comes from: its message in its chat (a chat that is open
// further back comes to the front instead of being opened once more), or
// its post.
void OpenTrackPlace(
	Window::SessionController *controller,
	const AudioTrack &track);

} // namespace Media
