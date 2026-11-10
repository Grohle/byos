#!/usr/bin/env python3
"""Checks a file written by the radio helper from the fake HLS server (tests/radio_helper_server.py):
it must be whole ADTS frames from the first byte (the last one may be cut), and the frame numbers
the server put in each payload must follow one another (no hole, no repeat).
Prints "frames=N first=K seconds=S ok=1|0 [reason]"."""
import sys

b = open(sys.argv[1], "rb").read()
i, n, prev, first, why = 0, 0, None, None, ""
while i + 7 <= len(b):
    if b[i] != 0xFF or (b[i + 1] & 0xF6) != 0xF0:
        why = "no ADTS sync at %d" % i
        break
    ln = ((b[i + 3] & 3) << 11) | (b[i + 4] << 3) | (b[i + 5] >> 5)
    if i + ln > len(b):
        break                                     # the frame being written
    p = b[i + 7:i + 11]
    k = ((p[0] * 200 + p[1]) * 200 + p[2]) * 200 + p[3]
    if prev is not None and k != prev + 1:
        why = "frame %d after %d" % (k, prev)
        break
    if first is None:
        first = k
    prev = k
    n += 1
    i += ln
print("frames=%d first=%s seconds=%.1f ok=%d %s" % (n, first, n * 1024 / 48000, int(not why and n > 0), why))
