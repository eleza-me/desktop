# Eleza Desktop — architecture

Native desktop client for Eleza (Windows + Linux), written from scratch: **C++20, Qt 6 Widgets
(no QML, no web view), CMake**, Eleza's own custom-painted UI library, and a small style language
(`.style` files) compiled to C++ at build time.

The reference client for behaviour and wire shapes is the web app (`eleza-me/pwa`, TypeScript); the
server contract is `eleza-backend`. Where docs and code disagree, the code wins.

## Layout

```
CMakeLists.txt  CMakePresets.json  vcpkg.json
cmake/                    helper modules (codegen rules, options)
codegen/
  style/                  host tool: *.style + colors.palette  ->  styles/style_*.h/.cpp, palette
  lang/                   host tool: lang.strings              ->  lang_auto.h/.cpp
  scripts/                dev-time generators (python): rt schema from views.proto, icons from icons.json
third_party/
  miniz/                  zip reading for the update packages (MIT), built from source
  livekit/                notices of the media library of voice calls (a prebuilt binary that
                          the build downloads and the app loads at run time; nothing is built)
lib/
  base/                   no-GUI helpers (bytes, weak guards, timers, random, platform file helpers)
  install/                plain C++, no Qt: replacing an installed copy's files with a new
                          version (journal, rollback, recovery) and the Windows Updater.exe
  crl/                    (none — Qt event loop only)
  ui/                     lib "ElezaUi": style runtime, animations, widgets, layers, text, icons, RTL
Eleza/
  Resources/              fonts, icons (svg), styles/*.style, colors.palette, langs/lang.strings, qrc
  SourceFiles/
    core/                 Application, Launcher, Settings, command line, single instance
    main/                 Main::Account (device + tokens) and Main::Session (signed-in services)
    api/                  HTTP client (request builder), errors, auth (OTP / two-step), client-config
    rt/                   realtime v2: noise, frames, dynamic protobuf codec + generated schema, client
    crypto/               OpenSSL wrappers: x25519, chacha20-poly1305, sha256/hmac/hkdf, device key
    storage/              per-account SQLite cache + key/value, secure blob for tokens / device key
    data/                 typed models + stores: chats, messages, users, sync (pts), outbox, media,
                          upload, feed, graph, conversations, notifications, stickers, calls, stories,
                          groups (their own sync track: data_groups.h; the decisions as plain
                          functions: data_group_rules.h)
    lang/                 lang runtime (fa / en), number + date formatting (Persian digits, Jalali)
    updater/              in-app updates without widgets: signed manifest, package unpacker,
                          the checker (check -> download -> verify -> unpack), install glue
    window/               MainWindow, SessionController (navigation), layout (nav rail | side | main)
    intro/                phone -> code -> password / recovery -> profile
    dialogs/              chat list
    history/              chat screen: list view, bubbles, composer, menus
    conversation/         anonymous talker/listener layer: topics, new/join sheets, radar
    groups/               groups beyond their chat: the info screen (members, settings), creating
                          a group, invite links, join requests, administration
    eli/                  feed, post cards, post detail, compose, notifications, topic pages, viewer
    search/               browse + search
    profile/              profiles, follow lists, edit profile
    settings/             settings sections
    boxes/                modal boxes shared by sections (peer list, photo crop, country list, about)
    stories/              stories: the tray on top of the feed, rings around faces, the
                          full-window viewer, posting, the viewers box, Settings → Privacy →
                          «استوری» (the data side: data/data_stories.h, data_story_types.h)
    media/                audio/voice player, video player widget, image viewer
    calls/                voice and video calls (ELEZA_ENABLE_CALLS): the call state machine, the
                          media layer over the LiveKit library, the camera, the call bar / view /
                          incoming window
    support/              shared session helpers: media images / userpics, error texts, links,
                          connection label, desktop notifications, file dialogs
tests/                    unit tests (noise vectors, proto codec, frames, style codegen)
```

Targets: `ElezaBase`, `ElezaUi` (static libs), `codegen_style`, `codegen_lang` (host tools),
`ElezaCore` (static: everything under SourceFiles without widgets: crypto, api, rt, storage, data,
main, lang), `ElezaUpdater` (static: SourceFiles/updater), `ElezaInstall` (static, no Qt: lib/install),
`ElezaMiniz`, `Eleza` (the executable: core/, window/ and all feature folders), `Updater` (Windows
only: Updater.exe, no Qt, static runtime), the tests.

## Updates

The app updates itself on Windows and Linux; `docs/RELEASING.md` describes the whole chain (the
signed `update.json`, the sources, the release workflow, the signing key). In the code:
`Updater::Checker` (`Core::App().updater()`) owns the state machine and the downloads,
`Updater::Unpack` reads the package as hostile input, `Install::Apply` (lib/install) swaps the files
— called by the exiting app on Linux and by `Updater.exe` on Windows — and the window shows it
through `window/window_update.*`, the settings page `settings/settings_update.cpp` and the
"update required" screen.

## Conventions

* Namespaces follow folders: `Ui::`, `style::` / `st::`, `Api::`, `Rt::`, `Data::`, `Main::`,
  `Window::`, `Intro::`, `Dialogs::`, `History::`, `Eli::`, `Conversation::`, `Settings::`…
* File names `snake_case.h/.cpp`; classes `PascalCase`; methods `camelCase`; members `_camelCase`.
* **Tabs** for indentation, `#pragma once`, no exceptions for control flow, no RTTI tricks.
* No `QString::fromUtf8("literal")` noise in UI code: user-visible strings come from `tr::lng_*(tr::now)`.
* No Qt stylesheets (QSS), no `.ui` files, no QML. Every widget paints itself from `st::` values.
* Sizes in `.style` are logical pixels (`px`), scaled by the device pixel ratio by Qt.
* Layout is **RTL-first** (Persian). Widgets position children through `Ui::` helpers
  (`moveToLeft/moveToRight` with an outer width, `style::RightToLeft()`), never hard-coded sides.
* Async: no blocking calls on the main thread. Network = `QNetworkAccessManager` / `QWebSocket`.
  Requests use the builder: `api.request(Api::Post, "/v1/…").body(json).done(fn).fail(fn).send()`;
  callbacks are dropped automatically when the owning `QObject` guard dies.
* Data objects are plain structs parsed once from JSON (`Data::Message::FromJson`), owned by the
  session stores; views observe stores through Qt signals and never keep wire JSON around.
* Rule from the realtime contract: **no polling**. A screen reads the local store first, fetches at
  most once what it does not hold, and then lives on `pts` updates / signals.
* The repository contains no references to third-party assistant tooling; authorship is `eleza`.

## Threads

Everything runs on the main (GUI) thread except: SQLite writes (one worker thread behind
`Storage::Database`), image decoding / scaling (`QThreadPool`), file hashing for uploads, and the
audio of a call (one "Calls media" thread that owns the microphone, the loudspeaker and every
call into the media library; `calls/calls_media_livekit.cpp`).

## Identity and platform string

Device key: Ed25519 or P-256 generated on first run, private key stored through
`Storage::SecureBlob` (DPAPI on Windows, 0600 file on Linux). Platform string sent to the API is
`desktop` (`X-Eleza-Client: desktop/<version> (<os>)`).

## Debug / test hooks

`docs/DEVELOPING.md` is the working guide for feature modules; its section 2 lists every hook
(`--api`, `--workdir`, `--debug-login`, `--debug-route`, `--debug-actions`, `--debug-grab`…),
the scripted input player and the `debug/…` routes. All of it except `--api`, `--workdir`,
`--lang`, `--theme`, `--no-single-instance`, `--version` and `--help` is compiled only with
the CMake option `ELEZA_DEBUG_TOOLS` (on for Debug builds, off for everything that is
shipped; `docs/BUILDING.md`).
