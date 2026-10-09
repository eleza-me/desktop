#!/bin/sh
# Builds Eleza Desktop in Release and packs it:
#
#   packaging/linux/build-deb.sh [build dir]        (default: build/package)
#
# Produces in <build dir>/dist:
#   eleza-desktop_<version>_<arch>.deb          for Debian / Ubuntu
#   Eleza-<version>-linux-<arch>.tar.gz         the same files as a plain tree (usr/…)
#
# Needs the build packages of docs/BUILDING.md plus dpkg-dev (for the .deb).
set -eu

root="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$root/build/package}"
dist="$build/dist"

cmake -S "$root" -B "$build" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DELEZA_BUILD_TESTS=OFF
cmake --build "$build"

version="$(sed -n 's/^CMAKE_PROJECT_VERSION:[^=]*=//p' "$build/CMakeCache.txt")"
arch="$(uname -m)"
mkdir -p "$dist"

stage="$build/stage"
rm -rf "$stage"
DESTDIR="$stage" cmake --install "$build" --strip
tar -C "$stage" -czf "$dist/Eleza-$version-linux-$arch.tar.gz" usr

if command -v dpkg-deb >/dev/null 2>&1; then
	ELEZA_BUILD_DIR="$build" cpack \
		--config "$root/packaging/linux/cpack-deb.cmake" \
		-B "$build/cpack"
	cp "$build"/cpack/*.deb "$dist/"
else
	echo "dpkg-deb not found: skipping the .deb (the tarball is ready)." >&2
fi

ls -l "$dist"
