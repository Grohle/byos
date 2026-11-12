#!/usr/bin/env python3
"""Record the panel while running a few tools/hiby.py actions, then flag glitches like tour.py.

    python tools/ondevice/rec.py NAME OUTDIR action [action ...]
    e.g. python tools/ondevice/rec.py volpop out/volpop key:106,event3 wait:3

Writes OUTDIR/NAME.fbr (the recording) and OUTDIR/NAME.png (contact sheet with flagged frames
in red) and prints a JSON summary. Needs the same setup as tour.py (HIBY_DEV, adb).
"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tour  # noqa: E402

if len(sys.argv) < 4:
    sys.exit(__doc__)
tour.DEV = os.environ.get("HIBY_DEV", "HiBy R3II_2025")
name, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
tour.OUT = out
r = tour.run_step(name, sys.argv[3:], out)
print(json.dumps({k: r[k] for k in ("frames", "seconds", "issues", "processes")}))
