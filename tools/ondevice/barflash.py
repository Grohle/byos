#!/usr/bin/env python3
"""Count status-bar flashes in recordings: frames where the status bar band (rows 0-29) changes
to something else and returns to what it was within a few frames (A -> B -> A).

    python tools/ondevice/barflash.py out/tour_fix [out/tour_r3 ...]

The clock or a battery/volume figure changing for good is not a flash (it does not come back).
"""
import glob, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbrec

def band(px, y0=0, y1=15):           # half-resolution rows (0-15 = full rows 0-30)
    return tuple(px[y * 160 + x] for y in range(y0, y1) for x in range(0, 160, 4))

def diff(a, b):
    return sum(1 for p, q in zip(a, b) if abs((p >> 11) - (q >> 11)) > 3 or abs(((p >> 5) & 63) - ((q >> 5) & 63)) > 6) / len(a)

def flashes(path, thr=0.08, back=3):
    fr = list(fbrec.decode(path)); out = []
    bands = [band(f['px']) for f in fr]
    for i in range(1, len(fr) - 1):
        if diff(bands[i], bands[i - 1]) < thr: continue
        for j in range(i + 1, min(len(fr), i + 1 + back)):
            if diff(bands[j], bands[i - 1]) < thr * 0.5 and fr[j]['t'] - fr[i]['t'] < 400:
                out.append((round(fr[i]['t']), j - i)); break
    return len(fr), out

for d in sys.argv[1:]:
    tot = 0; nfr = 0
    for p in sorted(glob.glob(os.path.join(d, '*.fbr'))):
        n, f = flashes(p); nfr += n; tot += len(f)
        if f: print('  %-30s %3d flashes  %s' % (os.path.basename(p), len(f), f[:6]))
    print('%s: %d frames, %d status-bar flashes' % (d, nfr, tot))
