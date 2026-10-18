Eleza Desktop for Linux (x86_64) — the portable build
=====================================================

Unpack the archive into a directory of its own (for example ~/Apps/Eleza) and
run ./Eleza from there.

It uses the Qt 6 of your system. On Ubuntu 24.04 / Debian 13 and newer:

    sudo apt install libqt6widgets6 libqt6network6 libqt6svg6 libqt6sql6-sqlite \
        libqt6multimedia6 libqt6websockets6 qt6-qpa-plugins qt6-image-formats-plugins \
        librlottie0-1

Updates: this copy keeps itself up to date. A new version is downloaded in the
background, checked against Eleza's signature and installed when you choose
«به‌روزرسانی الزا» ("Update Eleza"); Settings -> Updates has the switch. For
that the directory must stay writable by you. A copy installed from the .deb
package is updated by the system's package manager instead.

Voice calls use liblivekit_ffi.so, which lies next to the program (the LiveKit
client library; the notices of what it contains are in livekit_ffi-LICENSE.md).
Without that file Eleza works as before and says that calls are unavailable.
Sound goes through your system's audio server (PipeWire or PulseAudio).

Your data lives in ~/.local/share/eleza/Eleza and is not touched by updates.

https://eleza.me
