#!/bin/sh
# End to end on Linux: two copies of Eleza call each other through a real
# LiveKit server on this machine, with the call API of the backend played by
# tests/tools/mock-calls.mjs.
#
#   tests/tools/calls-e2e.sh <build dir> [work dir]
#
# <build dir> must be a build with the debug tools (a Debug build) and with
# calls (ELEZA_ENABLE_CALLS=ON). Needed besides it: node, python3, curl and
# the web client's checkout (ELEZA_PWA_DIR, or ../pwa next to this one).
#
# The media server is the pinned release of livekit-server: taken from
# LIVEKIT_SERVER (a path) or downloaded and verified into the work directory.
# When neither works (no network, another platform) the test SKIPS itself:
# exit code 0 and a line that says so.
#
# Nobody talks and nobody listens: each copy sends a test tone instead of the
# microphone (A 440 Hz, B 880 Hz; ELEZA_DEBUG_CALL_INPUT) and writes what it
# would play to a file (ELEZA_DEBUG_CALL_OUTPUT), which
# tests/tools/calls-audio-check.py then looks at. The copies are driven
# through files (ELEZA_DEBUG_CALL_CONTROL, see calls/calls_debug.cpp).
#
# What is checked:
#    1. a call: rings there, answered, both in the call; each side hears the
#       other's tone and not its own; the one-way delay; mute silences the
#       sender; the server got a quality report; hangup ends it for both;
#    2. declined by the callee;            3. cancelled by the caller;
#    4. the callee is busy;                5. the callee does not accept calls;
#    6. nobody answers (the server's ring timeout);
#    7. the audio connection is cut on the server during a call: the app asks
#       for fresh credentials (rejoin) and the audio comes back;
#    8. the app is killed during a call and started again: it finds its call
#       (calls/active) and joins it again;
#    9. no realtime connection on either side: the call still rings (found by
#       asking calls/incoming) and connects;
#   10. quitting during a call hangs up.
#
# Environment: PORT (8600: the two users' addresses are PORT and PORT + 1, the
# media server gets PORT + 90 .. PORT + 92), KEEP=1 (leave the work directory).
set -eu

if [ "$#" -lt 1 ]; then
	sed -n '2,39p' "$0" >&2
	exit 2
fi
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="$(cd "$1" && pwd)"
own_work=0
[ "$#" -ge 2 ] || own_work=1
work="${2:-$(mktemp -d)}"
mkdir -p "$work"
work="$(cd "$work" && pwd)"
port="${PORT:-8600}"
eleza="$build/Eleza/Eleza"

livekit_version=1.13.9
livekit_sha256=0b7fa208b662d09cfdeae8c06cf4c481aead0556086558b48250501e2e2d6e20
livekit_url="https://github.com/livekit/livekit/releases/download/v$livekit_version/livekit_${livekit_version}_linux_amd64.tar.gz"

skip() {
	echo "calls-e2e: SKIPPED: $*"
	exit 0
}

[ -x "$eleza" ] || { echo "calls-e2e: no $eleza" >&2; exit 2; }
for tool in node python3 curl; do
	command -v "$tool" >/dev/null 2>&1 || skip "$tool is not installed"
done

# ---- the media server

server="${LIVEKIT_SERVER:-}"
if [ -z "$server" ]; then
	[ "$(uname -s)-$(uname -m)" = "Linux-x86_64" ] \
		|| skip "no pinned livekit-server for $(uname -s) $(uname -m) (set LIVEKIT_SERVER)"
	server="$work/livekit/livekit-server"
	if [ ! -x "$server" ]; then
		mkdir -p "$work/livekit-download" "$work/livekit"
		archive="$work/livekit-download/livekit-server.tar.gz"
		curl -fsSL --retry 2 -o "$archive" "$livekit_url" \
			|| skip "livekit-server $livekit_version could not be downloaded"
		echo "$livekit_sha256  $archive" | sha256sum -c - >/dev/null 2>&1 \
			|| { echo "calls-e2e: the downloaded livekit-server does not match its pinned SHA-256" >&2; exit 1; }
		tar -xzf "$archive" -C "$work/livekit" livekit-server
	fi
fi
[ -x "$server" ] || skip "no livekit-server at $server"

pids=""
cleanup() {
	code=$?
	trap - EXIT INT TERM
	for pid in $pids; do
		kill "$pid" 2>/dev/null || true
	done
	wait 2>/dev/null || true
	if [ "$code" -ne 0 ]; then
		echo "calls-e2e: FAILED (logs in $work)" >&2
		for name in a b; do
			[ -f "$work/$name.log" ] && { echo "--- $name.log"; tail -n 15 "$work/$name.log"; } >&2
			[ -f "$work/$name/events" ] && { echo "--- $name events"; tail -n 12 "$work/$name/events"; } >&2
		done
		[ -f "$work/mock.log" ] && { echo "--- mock.log"; tail -n 20 "$work/mock.log"; } >&2
	elif [ "${KEEP:-0}" != "1" ] && [ "$own_work" -eq 1 ]; then
		rm -rf "$work"
	fi
	exit "$code"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

sfu_port=$((port + 90))
cat > "$work/livekit.yaml" <<YAML
port: $sfu_port
bind_addresses: ["127.0.0.1"]
rtc:
  tcp_port: $((port + 91))
  udp_port: $((port + 92))
  use_external_ip: false
  node_ip: 127.0.0.1
keys:
  devkey: secret
logging:
  level: info
YAML
"$server" --config "$work/livekit.yaml" > "$work/livekit.log" 2>&1 &
pids="$pids $!"

# ---- the two users' server

RING_TIMEOUT_MS=9000 PORT="$port" LIVEKIT_URL="ws://127.0.0.1:$sfu_port" \
	ELEZA_RTPEER_WORK="${ELEZA_RTPEER_WORK:-$work/rtpeer}" \
	node "$root/tests/tools/mock-calls.mjs" > "$work/mock.log" 2>&1 &
mock=$!
pids="$pids $mock"
tries=0
until grep -q '^READY' "$work/mock.log" 2>/dev/null; do
	if ! kill -0 "$mock" 2>/dev/null; then
		wait "$mock" && code=0 || code=$?
		pids=""
		[ "$code" -eq 77 ] && skip "$(tail -n 1 "$work/mock.log")"
		echo "calls-e2e: the mock did not start" >&2
		exit 1
	fi
	tries=$((tries + 1))
	[ "$tries" -lt 600 ] || { echo "calls-e2e: the mock is not ready" >&2; exit 1; }
	sleep 0.2
done
tries=0
until curl -fs -o /dev/null "http://127.0.0.1:$sfu_port/"; do
	tries=$((tries + 1))
	[ "$tries" -lt 100 ] || { echo "calls-e2e: livekit-server did not start" >&2; exit 1; }
	sleep 0.2
done
echo "== livekit-server $("$server" --version 2>/dev/null | sed 's/.*version //') on 127.0.0.1:$sfu_port, users on :$port and :$((port + 1))"

control="http://127.0.0.1:$port/control"
post() { # post <route> <json>
	curl -fsS -o /dev/null -X POST -H 'Content-Type: application/json' -d "$2" "$control/$1"
}
# server <call index from the end: -1 = the last> <field>
server_field() {
	curl -fsS "$control/calls" | python3 -c '
import json, sys
data = json.load(sys.stdin)
calls = data["calls"]
index, field = int(sys.argv[1]), sys.argv[2]
if field == "qos":
    print(sum(1 for report in data["qos"] if calls and report["call"] == calls[index]["id"]))
elif field == "count":
    print(len(calls))
else:
    print(calls[index][field] if calls else "")
' "$1" "$2"
}

# ---- the two apps

start_app() { # start_app <a|b> <port> <phone> <tone> [more environment as NAME=value…]
	name="$1"; shift
	app_port="$1"; shift
	phone="$1"; shift
	tone="$1"; shift
	mkdir -p "$work/$name"
	env QT_QPA_PLATFORM=offscreen \
		ELEZA_DEBUG_CALL_CONTROL="$work/$name" \
		ELEZA_DEBUG_CALL_INPUT="tone:$tone" \
		ELEZA_DEBUG_CALL_OUTPUT="$work/$name/heard.wav" \
		"$@" \
		"$eleza" --api="http://127.0.0.1:$app_port" --workdir="$work/wd-$name" \
		--no-single-instance --debug-login="$phone:123456" >> "$work/$name.log" 2>&1 &
	eval "pid_$name=$!"
	pids="$pids $!"
}
state_of() { # state_of <a|b> [field]
	python3 -c '
import json, sys
try:
    print(json.load(open(sys.argv[1], encoding="utf-8")).get(sys.argv[2], ""))
except (OSError, ValueError):
    print("")
' "$work/$1/state" "${2:-state}"
}
# wait_state <a|b> <state>[/<reason>] [seconds]
wait_state() {
	want="$2"
	limit=$(( ${3:-20} * 10 ))
	count=0
	while :; do
		now="$(state_of "$1")"
		reason="$(state_of "$1" reason)"
		[ "$now" = "$want" ] && return 0
		[ "$now/$reason" = "$want" ] && return 0
		count=$((count + 1))
		if [ "$count" -ge "$limit" ]; then
			echo "calls-e2e: $1 is '$now${reason:+/$reason}', expected '$want'" >&2
			return 1
		fi
		sleep 0.1
	done
}
# The call was shown as ended (how) at some point after the mark.
mark() { eval "mark_$1=\$(wc -l < \"$work/$1/events\" 2>/dev/null || echo 0)"; }
wait_event() { # wait_event <a|b> <text in an event line> [seconds]
	limit=$(( ${3:-20} * 10 ))
	count=0
	eval "from=\$mark_$1"
	until tail -n "+$((from + 1))" "$work/$1/events" 2>/dev/null | grep -q -- "$2"; do
		count=$((count + 1))
		if [ "$count" -ge "$limit" ]; then
			echo "calls-e2e: $1 never showed $2" >&2
			return 1
		fi
		sleep 0.1
	done
}
event_time() { # event_time <a|b> <text>: when the last such event after the mark was
	eval "from=\$mark_$1"
	tail -n "+$((from + 1))" "$work/$1/events" | grep -- "$2" | tail -n 1 | cut -d' ' -f1
}
tell() { # tell <a|b> <command…>
	name="$1"; shift
	count=0
	while [ -e "$work/$name/command" ]; do
		count=$((count + 1))
		[ "$count" -lt 50 ] || { echo "calls-e2e: $name does not take commands" >&2; return 1; }
		sleep 0.1
	done
	printf '%s\n' "$*" > "$work/$name/command.new"
	mv "$work/$name/command.new" "$work/$name/command"
}
expect() { # expect <what> <actual> <wanted>
	if [ "$2" != "$3" ]; then
		echo "calls-e2e: $1 is '$2', expected '$3'" >&2
		exit 1
	fi
}
idle() { # both sides are back to no call
	tell a dismiss; tell b dismiss
	wait_state a none 15
	wait_state b none 15
}
heard() { # heard <a|b> <expected Hz> <own Hz> <label> [more arguments]
	name="$1"; expect_hz="$2"; own_hz="$3"; label="$4"; shift 4
	# The first second is left out: the gain control is still settling on a
	# tone that starts at full level, and may clip its first burst.
	case " $* " in
	*" --from "*) ;;
	*) set -- "$@" --from 1 ;;
	esac
	cp "$work/$name/heard.wav" "$work/$label.wav"
	cp "$work/$name/heard.wav.json" "$work/$label.wav.json"
	python3 -I "$root/tests/tools/calls-audio-check.py" "$work/$label.wav" \
		--expect "$expect_hz" --reject "$own_hz" "$@" | sed 's/^/   /'
}
# Seconds into <a|b>'s recording at which the clock showed <ms>.
offset_in() {
	python3 -c '
import json, sys
started = json.load(open(sys.argv[1], encoding="utf-8"))["started_ms"]
print(max(int((float(sys.argv[2]) - started) / 1000.0), 0))
' "$work/$1/heard.wav.json" "$2"
}
connect_call() { # A calls, B answers, both are in the call
	mark a; mark b
	tell a start u_sara
	wait_state b incoming
	wait_event a '"state":"ringing"'
	tell b answer
	wait_state a in-call 30
	wait_state b in-call 30
}

start_app a "$port" +989121234567 440
start_app b "$((port + 1))" +989121234568 880
wait_state a none 60
wait_state b none 60
# The chats are there (the call button's rules need them).
sleep 3

echo "== 1. a call, answered"
t0=$(date +%s%3N)
mark a; mark b
tell a start u_sara
wait_state b incoming
t1=$(date +%s%3N)
wait_event a '"state":"ringing"'
tell b answer
wait_state a in-call 30
wait_state b in-call 30
t2=$(date +%s%3N)
echo "   rang at B after $((t1 - t0)) ms; both in the call $((t2 - t1)) ms after the answer"
expect "the server's state" "$(server_field -1 state)" connected
sleep 12
tell a mute
wait_event a '"muted":true'
muted_at=$(date +%s%3N)
sleep 6
tell b hangup
wait_state a ended/hangup
wait_event b '"state":"ended"'
expect "the server's end" "$(server_field -1 end_reason)" hangup
heard a 880 440 1-a-heard
muted_s="$(offset_in b "$muted_at")"
heard b 440 880 1-b-heard-before-mute --to "$muted_s"
# After the mute (and a second for what was on its way): silence at B.
if python3 -I "$root/tests/tools/calls-audio-check.py" "$work/1-b-heard-before-mute.wav" \
		--expect 440 --from "$((muted_s + 2))" > "$work/1-b-muted.txt" 2>&1; then
	echo "calls-e2e: B still heard A after A muted: $(cat "$work/1-b-muted.txt")" >&2
	exit 1
fi
echo "   ok: B heard nothing from A after A muted"
reports="$(server_field -1 qos)"
[ "$reports" -ge 1 ] || { echo "calls-e2e: the server got no quality report" >&2; exit 1; }
echo "   ok: quality reports the server got: $reports"
idle

echo "== 2. declined"
mark a; mark b
tell a start u_sara
wait_state b incoming
tell b hangup
wait_state a ended/declined
wait_state b none
expect "the server's end" "$(server_field -1 end_reason)" rejected
echo "   ok: A shows «declined», nothing is left at B, the server says rejected"
idle

echo "== 3. cancelled by the caller"
mark a; mark b
tell a start u_sara
wait_state b incoming
tell a hangup
wait_state b none
wait_state a none
expect "the server's end" "$(server_field -1 end_reason)" cancelled
echo "   ok: it stopped ringing at B, the server says cancelled"
idle

echo "== 4. busy"
post busy '{"user":"b","busy":true}'
before="$(server_field -1 count)"
mark a
tell a start u_sara
wait_state a ended/busy
expect "the number of calls on the server" "$(server_field -1 count)" "$before"
post busy '{"user":"b","busy":false}'
echo "   ok: A shows «busy», nothing rang"
idle

echo "== 5. the callee accepts no calls"
post privacy '{"user":"b","calls":"nobody"}'
mark a
tell a start u_sara
wait_state a ended/not-allowed
post privacy '{"user":"b","calls":"thread_contacts"}'
echo "   ok: A shows «not allowed»"
idle

echo "== 6. nobody answers"
mark a; mark b
tell a start u_sara
wait_state b incoming
wait_state a ended/no-answer 20
wait_state b none
expect "the server's end" "$(server_field -1 end_reason)" timeout
echo "   ok: A shows «no answer», it stopped ringing at B, the server says timeout"
idle

echo "== 7. the audio connection is cut during a call"
connect_call
sleep 3
mark a
post kick '{"user":"a"}'
wait_event a '"state":"reconnecting"' 15
wait_state a in-call 30
t0="$(event_time a '"state":"reconnecting"')"
t1="$(event_time a '"state":"in-call"')"
echo "   A was reconnecting for $((t1 - t0)) ms; rejoins asked: $(server_field -1 rejoins)"
[ "$(server_field -1 rejoins)" -ge 1 ] || { echo "calls-e2e: no rejoin was asked" >&2; exit 1; }
expect "B's state meanwhile" "$(state_of b)" in-call
sleep 7
tell a hangup
wait_event b '"state":"ended"'
wait_event a '"state":"ended"'
# A's recording starts with the new connection: what it holds came after it.
heard a 880 440 7-a-heard-after-rejoin
heard b 440 880 7-b-heard-after-rejoin --from "$(( $(offset_in b "$t1") + 1 ))"
idle

echo "== 8. the app is killed during a call and started again"
connect_call
sleep 2
eval "kill -9 \$pid_a"
sleep 1
rm -f "$work/a/state"
start_app a "$port" +989121234567 440
wait_event a '"state":"reconnecting"' 60
wait_state a in-call 40
expect "the server's state" "$(server_field -1 state)" connected
expect "B's state" "$(state_of b)" in-call
sleep 6
tell b hangup
wait_event a '"state":"ended"'
heard a 880 440 8-a-heard-after-restart
idle

echo "== 9. without the realtime connection"
eval "kill \$pid_a \$pid_b"
sleep 2
post signals '{"user":"a","down":true}'
post signals '{"user":"b","down":true}'
rm -f "$work/a/state" "$work/b/state"
start_app a "$port" +989121234567 440 ELEZA_DEBUG_CALL_INCOMING_INTERVAL_MS=2000
start_app b "$((port + 1))" +989121234568 880 ELEZA_DEBUG_CALL_INCOMING_INTERVAL_MS=2000
wait_state a none 60
wait_state b none 60
sleep 3
mark a; mark b
t0=$(date +%s%3N)
tell a start u_sara
wait_state b incoming 20
t1=$(date +%s%3N)
tell b answer
wait_state a in-call 30
wait_state b in-call 30
echo "   rang at B after $((t1 - t0)) ms (found by asking); connected"
sleep 6
tell a hangup
wait_event b '"state":"ended"' 20
heard a 880 440 9-a-heard
heard b 440 880 9-b-heard
idle

echo "== 10. quitting during a call"
connect_call
sleep 2
tell a quit
tries=0
while eval "kill -0 \$pid_a" 2>/dev/null; do
	tries=$((tries + 1))
	[ "$tries" -lt 100 ] || { echo "calls-e2e: A did not quit" >&2; exit 1; }
	sleep 0.1
done
expect "the server's end" "$(server_field -1 end_reason)" hangup
wait_event b '"state":"ended"' 20
echo "   ok: A left within $((tries * 100)) ms, the server says hangup, B's call ended"

echo "calls-e2e: PASSED"
