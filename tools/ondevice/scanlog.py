#!/usr/bin/env python3
"""Frame-exact flash finder for the HiBy screen.

The display library (src/hibymod_disp.c, only with /usr/data/hibymod/scanlog_on) samples four bands
of the buffer on screen every 2 ms (status bar, top, middle, home indicator) and logs every change in
/dev/shm/hibymod_scan, plus the events of whoever draws (HiBy pan captured/not, the shell's 'P',
'C' capture, 'R' release, 'O' overlay, same-buffer pan). This tool pulls that ring and reports
flashes: a band that changes and returns to its previous picture within 120 ms. The 13 fps screen
recorder misses those; this does not (2 ms sampling, the panel scans every ~22 ms).

Pictures of one band go to /dev/shm/hibymod_scanimg (48 most recent changes): the first byte of
scanlog_on picks it, re-read every second: "0" status bar (320x32), "3" home bar (320x32), "p" the
volume pill (32x160, x 288-320 y 40-200, stored in the first 5120 pixels of each slot).

    python tools/ondevice/scanlog.py [--dev SERIAL] [--clear] [--keep FILE]
      --clear   empty the ring (start of a test)
"""
import argparse, os, struct, subprocess, sys, tempfile

ADB = os.path.expandvars(r"%LOCALAPPDATA%\platform-tools\adb.exe")
SRC = {0: "sample", 1: "pan(capture)", 2: "pan(HiBy)", 3: "P", 4: "R", 5: "C", 6: "O", 7: "same-buffer pan"}
BANDS = ["status bar", "top", "middle", "home bar"]
REC = struct.Struct("<I4HBBH")


def pull(dev, out):
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    subprocess.run([ADB, "-s", dev, "pull", "/dev/shm/hibymod_scan", out], check=True, capture_output=True, env=env)


def parse(path):
    b = open(path, "rb").read()
    magic, n, idx, _ = struct.unpack_from("<IIII", b, 0)
    if magic != 0x5343414E: sys.exit("no scan log (is /usr/data/hibymod/scanlog_on set and the player restarted?)")
    recs = [REC.unpack_from(b, 16 + i * REC.size) for i in range(n)]
    count = min(idx, n); start = idx % n if idx > n else 0
    ordered = [recs[(start + i) % n] for i in range(count)]
    return [(t / 10.0, h, src) for (t, h0, h1, h2, h3, src, y, _p) in ordered for h in [(h0, h1, h2, h3)]]


def flashes(rows, window=120.0):
    samples = [(t, h) for t, h, s in rows if s == 0]
    events = [(t, s) for t, h, s in rows if s != 0]
    out = []
    for k in range(4):
        seq = [(t, h[k]) for t, h in samples]
        for i in range(1, len(seq) - 1):
            t0, prev = seq[i - 1]; t1, cur = seq[i]
            if cur == prev: continue
            for j in range(i + 1, len(seq)):
                tj, hj = seq[j]
                if tj - t1 > window: break
                if hj == prev:
                    ev = [SRC[s] for te, s in events if t0 - 30 <= te <= t1]
                    out.append((t1, BANDS[k], round(tj - t1, 1), ev[-3:]))
                    break
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dev", default=os.environ.get("HIBY_DEV", "HiBy R3II_2025"))
    ap.add_argument("--clear", action="store_true")
    ap.add_argument("--keep")
    a = ap.parse_args()
    if a.clear:
        env = dict(os.environ, MSYS_NO_PATHCONV="1")
        subprocess.run([ADB, "-s", a.dev, "shell", "printf '\\116\\101\\103\\123' > /dev/null"], env=env)
    path = a.keep or os.path.join(tempfile.gettempdir(), "hibymod_scan.bin")
    pull(a.dev, path)
    rows = parse(path)
    if not rows: print("empty"); return
    span = (rows[-1][0] - rows[0][0]) / 1000.0
    n_ev = {}
    for t, h, s in rows:
        if s: n_ev[SRC[s]] = n_ev.get(SRC[s], 0) + 1
    fl = flashes(rows)
    by = {}
    for f in fl: by[f[1]] = by.get(f[1], 0) + 1
    print("log: %.1f s, %d samples changed, events %s" % (span, sum(1 for r in rows if r[2] == 0), n_ev))
    print("FLASHES: %d  %s" % (len(fl), by))
    for f in fl[:40]: print("  t=%.1f ms  %-10s back after %5.1f ms  after %s" % f)


if __name__ == "__main__":
    main()
