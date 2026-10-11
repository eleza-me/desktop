# LiveKit FFI (the media library of voice calls)

Voice calls are carried by `livekit_ffi`, the client library of the LiveKit
project: a room client with its WebRTC stack behind a C interface of four
functions. Eleza does not contain its source and does not link against it. The
build downloads the prebuilt binary of one pinned release, checks its SHA-256
(`cmake/eleza_livekit.cmake`) and ships the file unchanged next to the
executable; the app loads it at run time (`Eleza/SourceFiles/calls/calls_ffi.cpp`)
and works without it, with calls reported as unavailable. Anybody can replace
the file with a build of their own of the same release.

| | |
|---|---|
| Project | https://github.com/livekit/rust-sdks (`livekit-ffi`) |
| Release | `rust-sdks/livekit-ffi@0.12.48` |
| Files | `ffi-windows-x86_64.zip` → `livekit_ffi.dll`, `ffi-linux-x86_64.zip` / `ffi-linux-arm64.zip` → `liblivekit_ffi.so` |
| Licence of the library | Apache License 2.0 |
| Shipped notices | `LICENSE.md` of the release archive, installed as `livekit_ffi-LICENSE.md` next to the library |

## What is inside the binary

The release archive's `LICENSE.md` (161 KB, shipped with every package) carries
the licence texts of the library and of everything its WebRTC build contains:

| Component | Licence |
|---|---|
| livekit (the client), perfetto, crc32c parts | Apache-2.0 |
| webrtc, abseil-cpp, libyuv, libvpx, libaom, libsrtp, pffft, rnnoise, opus, protobuf, crc32c, sigslot, g711 / g722, fft, ooura, spl_sqrt_floor | BSD-style / public domain |
| boringssl | OpenSSL / ISC / BSD-style / MIT |
| fiat (inside boringssl), zlib, libjpeg_turbo (IJG, BSD, zlib) | MIT / zlib-style |
| dav1d, openh264, nasm (build tool) | BSD-2-Clause |
| ffmpeg (the decoders WebRTC's build includes) | LGPL-2.1-or-later as configured by that build; the notices file reproduces all of FFmpeg's licence texts |

Besides those, the library's own (Rust) dependencies are compiled in; they are
MIT / Apache-2.0 / BSD licensed with one exception found in the binary: the
`soxr` resampler (LGPL-2.1-or-later).

So the library is not purely permissive: two LGPL components are statically
part of it. Eleza meets the LGPL's terms for them the same way it does for Qt:
the library is a separate file that the user can replace, its notices are
shipped, and its exact source is public at the release tag above (with the
WebRTC build it names in `webrtc-sys/build`). Eleza uses the library for audio
only; no video codec of it is ever called.

## Updating

Change the version and the three hashes in `cmake/eleza_livekit.cmake`
together, read the changes of `livekit-ffi/protocol/*.proto` between the two
tags (`calls_media_livekit.cpp` names every message field it uses), run the
tests (`test_calls_media_library` loads the new binary and talks to it) and
`tests/tools/calls-e2e.sh`, and look through the new `LICENSE.md` for
components that were not there before; update the table above and the
licences line of the About page (`lng_settings_about_licenses_text`).
