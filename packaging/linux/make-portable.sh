#!/bin/sh
# Assembles the portable Linux build from a build directory:
#
#   packaging/linux/make-portable.sh <build dir> <output dir> [archive.zip]
#
# <output dir> (created empty) gets the binary, the media library of voice
# calls with its licence notices (when the build has calls: both lie next to
# the built binary, cmake/eleza_livekit.cmake), the icon and a short README;
# with a third argument it is packed right away (packaging/make-package.py,
# which also writes eleza-files.list — the marker of a copy that updates
# itself). The archive is at once the download and the update package.
#
# The binary is stripped unless ELEZA_PORTABLE_STRIP=0.
set -eu

if [ "$#" -lt 2 ]; then
	echo "usage: $0 <build dir> <output dir> [archive.zip]" >&2
	exit 2
fi
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="$1"
output="$2"
binary="$build/Eleza/Eleza"
if [ ! -x "$binary" ]; then
	echo "make-portable: no binary at $binary" >&2
	exit 1
fi
if [ -e "$output" ] && [ -n "$(ls -A "$output" 2>/dev/null)" ]; then
	echo "make-portable: the output directory is not empty: $output" >&2
	exit 1
fi
mkdir -p "$output"
install -m 755 "$binary" "$output/Eleza"
if [ "${ELEZA_PORTABLE_STRIP:-1}" != "0" ]; then
	strip "$output/Eleza"
fi
# Voice calls: the app loads the library from its own directory. It is
# shipped as released (its hash is the pinned one), not stripped again.
if grep -q '^ELEZA_ENABLE_CALLS:BOOL=ON$' "$build/CMakeCache.txt" 2>/dev/null; then
	for name in liblivekit_ffi.so livekit_ffi-LICENSE.md; do
		if [ ! -f "$build/Eleza/$name" ]; then
			echo "make-portable: the build has calls but no $build/Eleza/$name" >&2
			exit 1
		fi
		install -m 644 "$build/Eleza/$name" "$output/$name"
	done
fi
install -m 644 "$root/Eleza/Resources/art/icon256.png" "$output/eleza.png"
install -m 644 "$root/packaging/linux/portable-readme.txt" "$output/README.txt"

if [ "$#" -ge 3 ]; then
	python3 "$root/packaging/make-package.py" "$output" "$3"
fi
