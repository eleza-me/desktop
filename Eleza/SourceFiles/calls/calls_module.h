// Voice calls (one to one, audio only), interoperable with the mobile apps.
//
//   data/data_calls.*          the server's side: commands and signals
//   calls_call.*               one call: the state machine          (no widgets)
//   calls_instance.*           which call there is                  (no widgets)
//   calls_media.h              the audio of a call, as an interface
//   calls_media_livekit.cpp    … over the media library (calls_ffi.*), with
//   calls_audio_io.*           the microphone and the loudspeaker
//   calls_service.*            the calls of the session: sounds, the incoming
//                              call window, settings on their way to the audio
//   calls_panel.*, calls_view.*, calls_incoming_window.*   what is on screen
//   calls_settings.cpp         «تماس‌ها» in the settings
//   calls_debug.cpp            "debug/calls/…" for screenshots (debug tools)
//
// The rest of the app starts a call through window/window_calls.h and knows
// nothing of this folder; with ELEZA_ENABLE_CALLS=OFF it is not compiled.
// docs/DEVELOPING.md, "Voice calls", has the test bed.
#pragma once

namespace Calls {

void Register();

} // namespace Calls
