# Building Eleza Desktop

C++20, Qt 6 Widgets (6.4 or newer), CMake 3.21+, Ninja, OpenSSL 3. Two host tools
(`codegen_style`, `codegen_lang`) are built first and generate the styles and the strings;
nothing else is downloaded during the build.

| | Linux | Windows |
| --- | --- | --- |
| Compiler | GCC 13 (Ubuntu 24.04) | MSVC 2022 (v143), x64 |
| Qt | the distribution's Qt 6 (6.4.2 on Ubuntu 24.04) | 6.7.3, the `msvc2019_64` kit |
| OpenSSL | `libssl-dev` | vcpkg manifest (`vcpkg.json`) |
| Status | built, run and tested by the authors | **built and tested only by CI** (see below) |

**About the Windows build.** The Windows steps below are the ones the CI job runs
(`.github/workflows/build.yml`: configure with the `windows-msvc-release` preset, build,
`ctest`, `windeployqt`, the installer). The authors have not yet built or run Eleza on a
Windows machine themselves: every command here was checked against the project's CMake files
and presets, but expect rough edges, and treat the first green CI run as the reference.
Please fix this file when something differs on a real machine.

What `docs/DEVELOPING.md` covers and this file does not: running against the mock API, the
debug flags, screenshots, how to add a module.

## Voice calls: the media library

Voice calls need one thing that is neither in the repository nor a system package: LiveKit's
`livekit_ffi` (the room client with its WebRTC stack; `third_party/livekit/README.md` says
what it contains and under which licences). `cmake/eleza_livekit.cmake` handles it:

* at configure time it downloads the prebuilt binary of the pinned release for the platform
  (`ffi-windows-x86_64.zip`, `ffi-linux-x86_64.zip` or `ffi-linux-arm64.zip`, 10–11 MB, from
  the project's GitHub releases), checks its SHA-256 and unpacks it under
  `<build>/_deps/`; a later configure finds it there and downloads nothing;
* after every build it copies `liblivekit_ffi.so` / `livekit_ffi.dll` (24–27 MB) and its
  notices (`livekit_ffi-LICENSE.md`) next to the executable. Nothing links against the
  library: the app loads it when the first call needs it, from its own directory or from
  `../lib/eleza` (an installed package), and without the file it runs as before and says that
  calls are unavailable.

| | |
|---|---|
| No network at configure time | download the archive elsewhere and pass `-DELEZA_LIVEKIT_FFI_ARCHIVE=<file>` (the hash is checked all the same) |
| No calls at all | `-DELEZA_ENABLE_CALLS=OFF`: nothing is downloaded, `Eleza/SourceFiles/calls` is not compiled, the app has no call buttons and no «تماس‌ها» settings |
| Another platform | the app is built with the call screens and reports calls as unavailable at run time (a CMake warning says so) |

The Linux binary needs glibc 2.28 or newer and the usual `libstdc++` / `libgcc_s`; the Windows
one imports system DLLs only (no C++ runtime DLLs). Sound devices are Qt Multimedia's: the
audio server of the system (PipeWire / PulseAudio, WASAPI).

## Linux

### Packages (Ubuntu 24.04)

```sh
sudo apt-get install \
  build-essential cmake ninja-build \
  qt6-base-dev qt6-base-dev-tools qt6-svg-dev qt6-multimedia-dev qt6-websockets-dev \
  libqt6sql6-sqlite qt6-image-formats-plugins \
  libssl-dev libgl1-mesa-dev libxkbcommon-dev
```

* `libqt6sql6-sqlite` is the SQLite driver of Qt SQL (the local cache); it is a run-time
  plugin, the build does not notice when it is missing.
* `qt6-image-formats-plugins` brings the WebP decoder: the server stores stickers as WebP.
  Without it stickers stay empty.
* `xvfb` is only needed to run the GUI tests under a real X server; they run on the
  `offscreen` platform by default.
* `ccache` is optional: add `-DCMAKE_CXX_COMPILER_LAUNCHER=ccache`.

Other distributions need the same Qt 6 modules: Core, Gui, Widgets, Network, Svg,
Multimedia, WebSockets, Sql (with the SQLite driver), Test; and OpenSSL 3 with headers.

### Build, test, run

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
./build/Eleza/Eleza
```

or with the presets (`build/linux-debug`, `build/linux-release`):

```sh
cmake --preset linux-debug && cmake --build --preset linux-debug && ctest --preset linux-debug
```

* The executable is `build/Eleza/Eleza`. It runs from the build directory: fonts, icons and
  strings are compiled in.
* `ELEZA_BUILD_TESTS=OFF` skips the tests; `ELEZA_WARNINGS_AS_ERRORS=ON` turns `-Wall
  -Wextra` warnings into errors.
* One test, `test_rt_interop`, talks to a peer built from the web client and needs `node`
  and a checkout of the web client next to this repository (`-DELEZA_PWA_DIR=…`); it reports
  itself as *skipped* otherwise.
* Without a display: `./build/Eleza/Eleza --version` checks that the binary and its
  libraries load; `QT_QPA_PLATFORM=offscreen timeout 5 ./build/Eleza/Eleza
  --no-single-instance --workdir=/tmp/wd` must run until the timeout ends it. In a build
  with the debug tools (below) `QT_QPA_PLATFORM=offscreen ./build/Eleza/Eleza --gallery
  --debug-grab=/tmp/gallery.png` renders the widget gallery to a file and exits.

### Debug tools (`ELEZA_DEBUG_TOOLS`)

One CMake option is the switch for every test / debug surface of the app:

* the flags `--debug-login`, `--debug-actions`, `--debug-route`, `--debug-grab`,
  `--debug-grab-delay`, `--debug-size`, `--debug-scale` and `--gallery` (with the gallery
  sources themselves);
* the `debug/…` routes of every module and the `intro/…` sample steps;
* the environment hooks `ELEZA_DEBUG_PICK`, `ELEZA_DEBUG_SAVE`, `ELEZA_DEBUG_FONTS`;
* a clear text (`http://`) API address outside the loopback interface;
* request paths, subjects (chat / user / post ids) and error texts in the log.

| `CMAKE_BUILD_TYPE` | default |
| --- | --- |
| `Debug`, or not given | `ON` |
| `Release`, `RelWithDebInfo`, `MinSizeRel` (every platform) | `OFF` |
| a multi-config generator (Visual Studio, Ninja Multi-Config) | `OFF` |

`-DELEZA_DEBUG_TOOLS=ON|OFF` overrides the default (the value is cached: pass it again
when an existing build directory changes its build type). With the option off the code is
not in the binary at all — `strings build/Eleza/Eleza | grep -c debug-login` prints 0 — a
`--debug-*` or `--gallery` argument prints one line (`… not available in this build
(ignored).`) and the app starts as usual. `--api=`, `--workdir=`, `--lang=`, `--theme=`,
`--no-single-instance`, `--version` and `--help` are in every build; so is `http://` to the
loopback interface (a local test server). **Whatever is shipped is built with the option
off**: the release presets, `packaging/linux/build-deb.sh` and the CI workflow say so
explicitly. The log of such a build has the method, the route template (`/v1/chats/:id/read`),
the status, the error code and the request id of a failed request — never ids, handles or
the server's message text.

### Install and packages

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build --strip
```

installs `bin/Eleza`, `share/applications/me.eleza.desktop.desktop`, the icons under
`share/icons/hicolor` and the media library of voice calls with its notices under
`lib/eleza`. For packages:

```sh
packaging/linux/build-deb.sh            # -> build/package/dist/
```

makes a Release build and writes `Eleza-<version>-linux-<arch>.zip` (the portable build: the
binary, the media library of voice calls with its notices, an icon and a README in a
directory of their own, see below) and, when `dpkg-deb` is
available, `eleza-desktop_<version>_<arch>.deb`. The `.deb` is described by
`packaging/linux/cpack-deb.cmake`; it can also be made from any configured build directory:

```sh
ELEZA_BUILD_DIR=$PWD/build cpack --config packaging/linux/cpack-deb.cmake -B build/cpack
```

Its dependencies on libraries come from `dpkg-shlibdeps`; the run-time plugins (SQLite
driver, platform plugins, image formats) are listed by hand in that file. The binaries link
the Qt of the machine they were built on: build on the oldest distribution you want to
support.

The portable build alone:

```sh
packaging/linux/make-portable.sh build out/Eleza out/Eleza-<version>-linux-x86_64.zip
```

It is the archive people download *and* the package the app updates itself from
(`docs/RELEASING.md`): `packaging/make-package.py` writes `eleza-files.list` into the
directory — the marker of a copy that updates itself — and packs it reproducibly. Unpacked
into a directory the user owns it keeps itself up to date; the `.deb` is updated by the
package manager and only announces new versions.

## Windows

### Tools

1. **Visual Studio 2022** (Community is enough) with the workload *Desktop development with
   C++*. It brings MSVC v143, the Windows SDK, CMake and Ninja.
2. **Qt 6.7.3 for MSVC 64-bit** — with the online installer: *Qt 6.7.3 → MSVC 2019 64-bit*
   plus, under *Additional Libraries*, **Qt Multimedia**, **Qt WebSockets** and **Qt Image
   Formats**. Or from a terminal with [aqtinstall](https://github.com/miurahr/aqtinstall):

   ```bat
   pip install aqtinstall
   aqt install-qt windows desktop 6.7.3 win64_msvc2019_64 -m qtmultimedia qtwebsockets qtimageformats -O C:\Qt
   ```

   The kit is called `msvc2019_64` up to Qt 6.7 and is the one to use with MSVC 2022 (the
   two are binary compatible); from Qt 6.8 on it is `msvc2022_64`. Qt SVG is part of the
   base package in Qt 6: there is no `qtsvg` module to add.
3. **vcpkg** for OpenSSL:

   ```bat
   git clone https://github.com/microsoft/vcpkg C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
   ```

   Nothing has to be installed by hand: the manifest (`vcpkg.json`) is resolved while CMake
   configures, into `build\windows-msvc-release\vcpkg_installed`. The first configure builds
   OpenSSL and takes several minutes.

### Build and test

From the **x64 Native Tools Command Prompt for VS 2022** (so that `cl`, `cmake` and `ninja`
are in `PATH`):

```bat
set VCPKG_ROOT=C:\vcpkg
set CMAKE_PREFIX_PATH=C:\Qt\6.7.3\msvc2019_64
set PATH=C:\Qt\6.7.3\msvc2019_64\bin;%PATH%

cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
```

The executable is `build\windows-msvc-release\Eleza\Eleza.exe`; it starts from there as long
as Qt's `bin` directory is in `PATH` (the OpenSSL DLLs are copied next to it by the vcpkg
toolchain). The build itself and `ctest` do not need Qt in `PATH`: the code generators and
the tests are started with Qt's `bin` directory prepended (CMake 3.25+ for the generators,
3.22+ for the tests; with an older CMake keep the `set PATH` line above).

`ELEZA_DEBUG_TOOLS` (every debug flag and route, see "Debug tools" in the Linux section and
`docs/DEVELOPING.md`) is off in the `windows-msvc-release` preset, as in every build that is
not Debug. For a build with the hooks configure a Debug build or pass
`-DELEZA_DEBUG_TOOLS=ON`. The scripted input of `--debug-actions` calls two functions QtGui
exports for QtTest only, declared by hand in `core/core_debug.cpp`: should a Qt version
change them, the link of such a build fails there (a build without the option is not
affected).

The custom window frame (`window/window_title_win.cpp`, compiled on Windows only: sources
named `*_win.cpp` are left out elsewhere) links `dwmapi`, `user32` and `shell32` — system
import libraries of the Windows SDK, nothing to install or deploy. It needs Windows 10 1607
or later and the Qt platform plugin of Qt 6.7 or later; with an older Qt the app keeps the
title bar of the system (`docs/DEVELOPING.md`, "The window frame").

### Deploying (a folder that runs anywhere)

```bat
mkdir dist\Eleza
copy build\windows-msvc-release\Eleza\Eleza.exe dist\Eleza\
copy build\windows-msvc-release\Eleza\Updater.exe dist\Eleza\
copy build\windows-msvc-release\Eleza\livekit_ffi.dll dist\Eleza\
copy build\windows-msvc-release\Eleza\livekit_ffi-LICENSE.md dist\Eleza\
windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime dist\Eleza\Eleza.exe
copy build\windows-msvc-release\vcpkg_installed\x64-windows\bin\libcrypto-3-x64.dll dist\Eleza\
copy build\windows-msvc-release\vcpkg_installed\x64-windows\bin\libssl-3-x64.dll dist\Eleza\
copy "%VCToolsRedistDir%x64\Microsoft.VC143.CRT\*.dll" dist\Eleza\
```

`Updater.exe` is the small program that swaps the files when the app updates itself
(`lib/install`, no Qt, static C++ runtime: it must not need the DLLs it replaces).
`livekit_ffi.dll` is the media library of voice calls ("Voice calls: the media library"
above); a folder without it works, with calls reported as unavailable. Then:

```bat
python packaging\make-package.py dist\Eleza dist\Eleza-0.4.0-windows-x64.zip
```

writes `dist\Eleza\eleza-files.list` (without it and `Updater.exe` a copy does not update
itself) and packs the folder: the zip is the portable build and the update package. The
installer is made from the same folder with
[Inno Setup](https://jrsoftware.org/isinfo.php) 6.3 or newer:

```bat
iscc /DAppVersion=0.4.0 /DSourceDir=%CD%\dist\Eleza /DOutputDir=%CD%\dist\installer packaging\windows\eleza.iss
```

It installs per user into `%LOCALAPPDATA%\Programs\Eleza` (no administrator rights, so the
app can update itself there silently; `/ALLUSERS` on the command line installs under Program
Files instead), adds a Start menu entry, offers a desktop icon and "start when I sign in",
and registers an uninstaller. User data lives in
`%APPDATA%\eleza\Eleza` and is left alone by upgrades and by the uninstaller.

### Common pitfalls

* **`VCPKG_ROOT` points at the wrong vcpkg.** The developer prompt of Visual Studio 2022
  sets `VCPKG_ROOT` to the copy bundled with Visual Studio. The preset reads
  `%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake`: set the variable to your own checkout
  *after* opening the prompt (the CI job does the same). Symptom: `Could NOT find OpenSSL`
  or a vcpkg error about a missing baseline.
* **OpenSSL.** `find_package(OpenSSL 3 REQUIRED)` must find the vcpkg one; an old OpenSSL 1.1
  from another program in `PATH` or `OPENSSL_ROOT_DIR` breaks the configure step. At run
  time `libcrypto-3-x64.dll` and `libssl-3-x64.dll` must sit next to `Eleza.exe`: both our
  own crypto and Qt's OpenSSL TLS backend load them.
* **`/utf-8`.** The sources and the generated strings contain Persian text in UTF-8.
  `cmake/eleza_options.cmake` passes `/utf-8` to every target; a target that misses the flag
  compiles on an English system and shows mojibake (or fails with warning C4819) on others.
  Keep new targets inside the project's CMake tree, and save files as UTF-8 without BOM.
* **The SQLite plugin.** The local cache uses Qt SQL with the `QSQLITE` driver, a plugin:
  `sqldrivers\qsqlite.dll`. `windeployqt` copies it because the executable links Qt6Sql; if
  it is missing Qt logs *QSQLITE driver not loaded* and the local cache cannot be opened.
* **The TLS plugins.** HTTPS and the realtime connection need `tls\qopensslbackend.dll` or
  `tls\qschannelbackend.dll`. Symptom of a missing `tls` folder: every request fails with
  *TLS initialization failed*.
* **The multimedia backend.** Voice messages, video and sounds use Qt Multimedia, whose
  backend is a plugin: `multimedia\ffmpegmediaplugin.dll` (the default, needs the
  `avcodec-*.dll`, `avformat-*.dll`, `avutil-*.dll`, `swresample-*.dll`, `swscale-*.dll`
  files from Qt's `bin` directory next to the executable) or
  `multimedia\windowsmediaplugin.dll`. Some `windeployqt` versions leave the FFmpeg DLLs
  out: copy them by hand (the CI job does). `set QT_MEDIA_BACKEND=windows` switches to the
  Media Foundation backend to tell a backend problem from an application one.
* **Image formats.** `imageformats\qwebp.dll` (stickers), `qjpeg.dll`, `qgif.dll`,
  `qsvg.dll` (the icons are SVG: without it the window is full of empty squares). `qwebp`
  only exists when the *Qt Image Formats* module was installed.
* **Running tests.** The tests are plain executables that need the Qt DLLs: `ctest` adds
  Qt's `bin` to `PATH` for them; to start one by hand keep it in `PATH` yourself. GUI tests
  set `QT_QPA_PLATFORM=offscreen` themselves.
* **The offscreen platform in a deployed folder.** `windeployqt` copies only
  `platforms\qwindows.dll`. To run the deployed `Eleza.exe` with
  `QT_QPA_PLATFORM=offscreen` (the CI smoke test does) point
  `QT_QPA_PLATFORM_PLUGIN_PATH` at `<Qt>\plugins\platforms`.
* **Line endings.** `.gitattributes` keeps LF in the working tree on Windows too; the test
  data is compared byte by byte. Do not override it with a global `core.autocrlf` rewrite
  of an existing checkout.
* **A new `.rc` file or compiler option.** Options added with `add_compile_options` /
  `target_compile_options` also reach `rc.exe` for targets that have a `.rc` file; wrap
  MSVC options in `$<$<COMPILE_LANGUAGE:C,CXX>:…>` as `cmake/eleza_options.cmake` does.
* **Debug builds.** The preset is Release with the release runtime (`/MD`). For a Debug
  build configure your own preset with `CMAKE_BUILD_TYPE=Debug` and
  `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebugDLL` (see `CMakeUserPresets.json`, which is
  ignored by git), and deploy with `windeployqt --debug`.

## Continuous integration

`.github/workflows/build.yml` builds both platforms on every push to `main` and on pull
requests (and for a `v*` tag as a part of `release.yml`, which then signs the update
manifest and publishes the release: `docs/RELEASING.md`), runs `ctest` (which also loads the
media library of voice calls on that platform: `test_calls_media_library`), checks that the
packages carry that library, starts the built binary once (`--version`; on
Linux also a few seconds on the offscreen platform) and uploads the packages. Both jobs
build Release with `ELEZA_DEBUG_TOOLS` off — the tests do not need the debug flags — so the
artifacts have no debug hooks; the Linux job checks that:

* `Eleza-<version>-windows-x64-portable` — `Eleza-<version>-windows-x64.zip`, the deployed
  folder;
* `Eleza-<version>-windows-x64-setup` — the installer;
* `Eleza-<version>-linux-x86_64` — `Eleza-<version>-linux-x86_64.zip` (the portable build)
  and the `.deb`.

The version is the one of `project(Eleza VERSION …)` in the root `CMakeLists.txt` (keep
`vcpkg.json` in step with it).
