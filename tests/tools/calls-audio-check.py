#!/usr/bin/env python3
"""Checks what one side of a test call heard.

    python3 tests/tools/calls-audio-check.py <recording.wav> --expect <Hz>
        [--reject <Hz>] [--from <s>] [--to <s>] [--min-level <0..1>] [--json]

The recording is what Eleza (or calls_media_probe) writes instead of playing
when its output is replaced for a test: 48 kHz mono, with <recording.wav>.json
next to it holding the clock time of the first sample. The other side sent the
test tone ("tone:<Hz>"): a sine that sounds during the first half of every
second of the system clock.

What is measured, in windows of 20 ms (the path stretches and squeezes time a
little, so nothing is compared over longer spans):

  * heard     the share of the windows that should carry the tone (by the
              recording's own on/off pattern) in which <Hz> dominates;
  * purity    the tone's share of the energy in those windows;
  * foreign   the level of the --reject tone (the listener's own tone must not
              come back to it);
  * latency   when the tone starts in the recording, relative to the full
              second of the clock at which the sender started it: the one-way
              delay from the sender's "microphone" to the listener's
              "loudspeaker" (both ends are on this machine's clock).

Exit code 0 when the tone was heard (heard >= 0.8, purity >= 0.5, foreign low),
1 otherwise. Only the standard library is used.
"""

import argparse
import json
import math
import struct
import sys
import wave

WINDOW = 960  # 20 ms at 48 kHz


def goertzel_power(samples, frequency, rate):
    w = 2.0 * math.pi * frequency / rate
    coefficient = 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for value in samples:
        s0 = value + coefficient * s1 - s2
        s2 = s1
        s1 = s0
    power = s1 * s1 + s2 * s2 - coefficient * s1 * s2
    # Normalised so that a full-window sine of amplitude A gives A*A/2 (its
    # mean square), comparable with the window's total energy.
    return max(power, 0.0) * 2.0 / (len(samples) * len(samples))


def analyse(path, expect, reject, start, end):
    with wave.open(path, "rb") as stream:
        if stream.getnchannels() != 1 or stream.getsampwidth() != 2:
            raise SystemExit("calls-audio-check: 16-bit mono expected: " + path)
        rate = stream.getframerate()
        raw = stream.readframes(stream.getnframes())
    samples = struct.unpack("<%dh" % (len(raw) // 2), raw)
    started_ms = None
    try:
        with open(path + ".json", "r", encoding="utf-8") as sidecar:
            started_ms = float(json.load(sidecar)["started_ms"])
    except (OSError, ValueError, KeyError):
        pass

    first = int(start * rate)
    last = len(samples) if end is None else min(int(end * rate), len(samples))
    windows = []
    for offset in range(first, last - WINDOW + 1, WINDOW):
        chunk = samples[offset:offset + WINDOW]
        energy = sum(value * value for value in chunk) / float(WINDOW)
        tone = goertzel_power(chunk, expect, rate)
        foreign = goertzel_power(chunk, reject, rate) if reject else 0.0
        windows.append((offset, energy, tone, foreign))
    result = {
        "file": path,
        "seconds": round(len(samples) / float(rate), 2),
        "windows": len(windows),
        "heard": 0.0,
        "purity": 0.0,
        "level": 0.0,
        "foreign_level": 0.0,
        "latency_ms": None,
    }
    if not windows:
        return result

    peak_tone = max(tone for _, _, tone, _ in windows)
    if peak_tone <= 0.0:
        return result
    # Windows in which the tone is clearly on: above a quarter of its peak
    # amplitude. Half of the time it is off by design.
    threshold = peak_tone / 16.0
    on = [window for window in windows if window[2] >= threshold]
    loud = [window for window in windows
            if window[1] >= threshold]  # anything as loud as that
    if loud:
        result["heard"] = round(len(on) / float(len(loud)), 3)
    if on:
        tone_energy = sum(tone for _, _, tone, _ in on)
        total_energy = sum(energy for _, energy, _, _ in on)
        result["purity"] = round(tone_energy / total_energy, 3) if total_energy else 0.0
        result["level"] = round(
            math.sqrt(2.0 * tone_energy / len(on)) / 32768.0, 4)
    result["on_share"] = round(len(on) / float(len(windows)), 3)
    if reject:
        foreign_peak = max(foreign for _, _, _, foreign in windows)
        result["foreign_level"] = round(math.sqrt(2.0 * foreign_peak) / 32768.0, 4)

    # Latency: the starts of the tone (off for at least 200 ms, then on).
    if started_ms is not None:
        delays = []
        quiet = 0
        for offset, _, tone, _ in windows:
            if tone >= threshold:
                if quiet >= 10:
                    # Finer than a window: the first sample above a tenth of
                    # the tone's amplitude around the start of this window.
                    amplitude = math.sqrt(2.0 * peak_tone)
                    begin = max(offset - WINDOW, 0)
                    onset = offset
                    for index in range(begin, min(offset + WINDOW, len(samples))):
                        if abs(samples[index]) >= amplitude / 10.0:
                            onset = index
                            break
                    at_ms = started_ms + onset * 1000.0 / rate
                    delays.append(at_ms % 1000.0)
                quiet = 0
            else:
                quiet += 1
        if delays:
            delays.sort()
            result["latency_ms"] = round(delays[len(delays) // 2], 1)
            result["latency_samples"] = len(delays)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("recording")
    parser.add_argument("--expect", type=float, required=True)
    parser.add_argument("--reject", type=float, default=0.0)
    parser.add_argument("--from", dest="start", type=float, default=0.0)
    parser.add_argument("--to", dest="end", type=float, default=None)
    parser.add_argument("--min-level", type=float, default=0.01)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    result = analyse(args.recording, args.expect, args.reject, args.start, args.end)
    ok = (result["heard"] >= 0.8
          and result["purity"] >= 0.5
          and result["level"] >= args.min_level
          and result["foreign_level"] <= max(result["level"] / 4.0, 0.002))
    result["ok"] = ok
    if args.json:
        print(json.dumps(result))
    else:
        latency = result["latency_ms"]
        print("%s: %s  tone %g Hz heard=%.2f purity=%.2f level=%.3f foreign=%.4f latency=%s" % (
            "ok" if ok else "FAILED",
            result["file"].split("/")[-1],
            args.expect,
            result["heard"],
            result["purity"],
            result["level"],
            result["foreign_level"],
            "n/a" if latency is None else "%.0f ms" % latency))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
