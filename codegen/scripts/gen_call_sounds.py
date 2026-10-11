#!/usr/bin/env python3
"""Synthesises the sounds of voice calls.

    python3 codegen/scripts/gen_call_sounds.py     # -> Eleza/Resources/sounds/call_*.wav

    call_ringtone.wav   an incoming call: a rising three-note figure played
                        twice, then a pause; looped while it rings
    call_ringback.wav   my call rings on their device: one soft double tone,
                        then a long pause; looped
    call_connect.wav    the call is connected: two quick rising notes
    call_end.wav        the call ended: two quick falling notes
    call_busy.wav       busy / declined: three low beeps

Like the notification sound (gen_notify_sound.py) everything is made of sine
notes with a quick attack and an exponential decay, in the same family of
pitches; mono, 22.05 kHz, 16 bit. The files are committed; run the script
again only to change a sound.
"""
import math
import os
import struct
import sys
import wave

RATE = 22050

E5, G5, A5, B5, C6, D6, E6 = 659.25, 783.99, 880.00, 987.77, 1046.50, 1174.66, 1318.51
A4, E4 = 440.00, 329.63


def note(start, frequency, length, level, decay=7.0, partial=0.18):
    return (start, frequency, length, level, decay, partial)


SOUNDS = {
    "call_ringtone": (3.0, 0.8, [
        note(0.00, E5, 0.30, 0.45), note(0.16, A5, 0.30, 0.50), note(0.32, C6, 0.55, 0.55, 5.0),
        note(0.95, E5, 0.30, 0.45), note(1.11, A5, 0.30, 0.50), note(1.27, E6, 0.75, 0.50, 4.0),
    ]),
    "call_ringback": (4.0, 0.5, [
        note(0.00, A4, 0.55, 0.40, 2.5, 0.08), note(0.00, E5, 0.55, 0.22, 2.5, 0.0),
        note(0.70, A4, 0.75, 0.40, 2.2, 0.08), note(0.70, E5, 0.75, 0.22, 2.2, 0.0),
    ]),
    "call_connect": (0.42, 0.7, [
        note(0.00, A5, 0.14, 0.45, 14.0), note(0.09, E6, 0.30, 0.50, 11.0),
    ]),
    "call_end": (0.46, 0.7, [
        note(0.00, A5, 0.14, 0.45, 14.0), note(0.10, E5, 0.34, 0.50, 10.0),
    ]),
    "call_busy": (1.30, 0.6, [
        note(0.00, E4, 0.24, 0.50, 3.0, 0.10), note(0.40, E4, 0.24, 0.50, 3.0, 0.10),
        note(0.80, E4, 0.24, 0.50, 3.0, 0.10),
    ]),
}


def envelope(t, length, decay):
    if t < 0.0 or t > length:
        return 0.0
    attack = min(t / 0.010, 1.0)
    release = min((length - t) / 0.030, 1.0)
    return attack * math.exp(-t * decay) * release


def render(total, gain, notes):
    frames = bytearray()
    for index in range(int(RATE * total)):
        t = index / RATE
        value = 0.0
        for start, frequency, length, level, decay, partial in notes:
            local = t - start
            amount = envelope(local, length, decay) * level
            if amount <= 0.0:
                continue
            value += amount * (
                math.sin(2.0 * math.pi * frequency * local)
                + partial * math.sin(2.0 * math.pi * frequency * 2.0 * local))
        value = max(-1.0, min(1.0, value))
        frames += struct.pack("<h", int(value * 32767 * gain))
    return bytes(frames)


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    directory = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        root, "Eleza", "Resources", "sounds")
    for name, (total, gain, notes) in sorted(SOUNDS.items()):
        target = os.path.join(directory, name + ".wav")
        frames = render(total, gain, notes)
        with wave.open(target, "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(RATE)
            output.writeframes(frames)
        print("%s: %d samples" % (target, len(frames) // 2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
