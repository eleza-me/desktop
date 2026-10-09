#!/usr/bin/env python3
"""Synthesises the message notification sound: a soft two-tone blip.

    python3 codegen/scripts/gen_notify_sound.py            # -> Eleza/Resources/sounds/notify.wav

Two sine notes a fourth apart (with a quiet octave partial for warmth), each
with a quick attack and an exponential decay; mono, 44.1 kHz, 16 bit. The file
is committed; run the script again only to change the sound.
"""
import math
import os
import struct
import sys
import wave

RATE = 44100
NOTES = (
    # start (s), frequency (Hz), length (s), level
    (0.000, 659.25, 0.16, 0.50),  # E5
    (0.085, 880.00, 0.26, 0.55),  # A5
)
TOTAL = 0.38


def envelope(t: float, length: float) -> float:
    if t < 0.0 or t > length:
        return 0.0
    attack = min(t / 0.008, 1.0)
    decay = math.exp(-t * 14.0)
    release = min((length - t) / 0.02, 1.0)
    return attack * decay * release


def sample(t: float) -> float:
    value = 0.0
    for start, frequency, length, level in NOTES:
        local = t - start
        gain = envelope(local, length) * level
        if gain <= 0.0:
            continue
        value += gain * (
            math.sin(2.0 * math.pi * frequency * local)
            + 0.18 * math.sin(2.0 * math.pi * frequency * 2.0 * local)
        )
    return max(-1.0, min(1.0, value))


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    target = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        root, "Eleza", "Resources", "sounds", "notify.wav")
    frames = bytearray()
    for index in range(int(RATE * TOTAL)):
        frames += struct.pack("<h", int(sample(index / RATE) * 32767 * 0.8))
    with wave.open(target, "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(RATE)
        output.writeframes(bytes(frames))
    print(f"{target}: {len(frames) // 2} samples")
    return 0


if __name__ == "__main__":
    sys.exit(main())
