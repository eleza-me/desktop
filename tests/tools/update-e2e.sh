#!/bin/sh
# End to end on Linux: a portable copy of the version in a build directory
# finds a signed update on a local server, downloads, verifies and unpacks it,
# swaps its own files and comes up again as the new version.
#
#   tests/tools/update-e2e.sh <build dir> [work dir]
#
# <build dir> must be a build with the debug tools (a Debug build): the test
# release is served over http on the loopback interface and signed with a key
# generated here (--update-url / --update-key / --update-now).
#
# "The next version" is the same source built with -DELEZA_TEST_VERSION in the
# same build directory (with ccache only the few files that use the version are
# compiled again); the directory is configured back at the end.
#
# What is checked:
#   1. a manifest that was changed after signing is refused: nothing is
#      downloaded, the copy stays what it was;
#   2. the good release: the installed copy reports the new version, the
#      process that was started is still there (the same pid, now running the
#      new binary with the arguments it was given), the backup and the staging
#      directory are gone;
#   3. the new version finds itself up to date.
set -eu

if [ "$#" -lt 1 ]; then
	sed -n '2,24p' "$0" >&2
	exit 2
fi
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="$(cd "$1" && pwd)"
work="${2:-$(mktemp -d)}"
mkdir -p "$work"
work="$(cd "$work" && pwd)"

old="$("$build/Eleza/Eleza" --version | sed 's/.* //')"
new="$(echo "$old" | awk -F. '{ printf "%d.%d.%d", $1, $2 + 1, 0 }')"
echo "== $old -> $new, in $work"

server=""
app=""
cleanup() {
	[ -n "$app" ] && kill "$app" 2>/dev/null || true
	[ -n "$server" ] && kill "$server" 2>/dev/null || true
	# The build directory becomes the developer's own version again.
	cmake -S "$root" -B "$build" -DELEZA_TEST_VERSION= >/dev/null
	cmake --build "$build" --target Eleza >/dev/null
}
trap cleanup EXIT INT TERM

echo "== the installed copy ($old)"
rm -rf "$work/app" "$work/next" "$work/served" "$work/data"
ELEZA_PORTABLE_STRIP=1 "$root/packaging/linux/make-portable.sh" "$build" "$work/app" "$work/installed-$old.zip"

echo "== building $new"
cmake -S "$root" -B "$build" "-DELEZA_TEST_VERSION=$new" >/dev/null
cmake --build "$build" --target Eleza >/dev/null
test "$("$build/Eleza/Eleza" --version)" = "Eleza Desktop $new"
"$root/packaging/linux/make-portable.sh" "$build" "$work/next"

port="$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
base="http://127.0.0.1:$port"
echo "== the test release at $base"
public="$("$root/tests/tools/make-test-update.sh" "$new" "$work/next" "$base" "$work/served" | tee /dev/stderr | sed -n 's/^public key: //p')"
python3 -m http.server "$port" --bind 127.0.0.1 --directory "$work/served" > "$work/server.log" 2>&1 &
server=$!
sleep 1

run_app() {
	QT_QPA_PLATFORM=offscreen "$work/app/Eleza" \
		--api=http://127.0.0.1:9 \
		"--workdir=$work/data" \
		--no-single-instance \
		"--update-url=$base/update.json" \
		"--update-key=$public" \
		--update-now > "$work/app.log" 2>&1 &
	app=$!
}
installed() {
	"$work/app/Eleza" --version | sed 's/.* //'
}

echo "== 1. a manifest changed after it was signed"
cp "$work/served/update.json" "$work/update.json.good"
sed "s/\"version\": \"$new\"/\"version\": \"9.9.9\"/" "$work/update.json.good" > "$work/served/update.json"
run_app
sleep 8
kill "$app" 2>/dev/null || true
wait "$app" 2>/dev/null || true
app=""
grep -h "signature" "$work/app.log" || true
test "$(installed)" = "$old"
if grep -q '\.zip' "$work/server.log"; then
	echo "FAILED: the package was requested for a manifest that does not verify" >&2
	exit 1
fi
test ! -e "$work/data/updates/$new"
echo "   refused; still $(installed), the package was never requested"

echo "== 2. the signed release"
cp "$work/update.json.good" "$work/served/update.json"
run_app
started=$app
waited=0
while [ "$(installed)" != "$new" ] || [ -e "$work/data/updates/install.result" ]; do
	waited=$((waited + 1))
	if [ "$waited" -gt 120 ]; then
		echo "FAILED: still $(installed) after two minutes" >&2
		cat "$work/app.log" "$work/data/updates/install.log" 2>/dev/null >&2 || true
		exit 1
	fi
	sleep 1
done
sleep 3
echo "   the installed copy reports: $("$work/app/Eleza" --version)"
echo "   pid $started is running: $(readlink "/proc/$started/exe")"
echo "   with the arguments: $(tr '\0' ' ' < "/proc/$started/cmdline")"
test "$(readlink "/proc/$started/exe")" = "$work/app/Eleza"
tr '\0' '\n' < "/proc/$started/cmdline" | grep -q -- '--update-now'
echo "   the install log:"
sed 's/^/     /' "$work/data/updates/install.log"
echo "   requests the server saw:"
sed -n 's/.*"\(GET [^"]*\)" \([0-9]*\).*/     \1 -> \2/p' "$work/server.log"
echo "   the install directory:"
ls -A "$work/app" | sed 's/^/     /'
if ls -A "$work/app" | grep -q '^_'; then
	echo "FAILED: the swap left something behind" >&2
	exit 1
fi
test ! -e "$work/data/updates/$new"

echo "== 3. the new version checks again"
sleep 4
grep -c "GET /update.json " "$work/server.log" | sed 's/^/   manifest requests so far: /'
test "$(installed)" = "$new"
kill "$started"
wait "$started" 2>/dev/null || true
app=""
echo "== passed: $old updated itself to $new"
