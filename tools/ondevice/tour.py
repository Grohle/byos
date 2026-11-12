#!/usr/bin/env python3
"""On-device tour tester for ByOS: drives the player, records what the panel shows and flags
glitches. It grows with the system: add steps to TOURS (or a JSON file) as features appear.

How it works
  * `hibyctl fbrec 5601 2 4000` on the player polls the buffer the panel is scanning every 4 ms
    and streams every changed frame (half resolution, XOR + RLE) over `adb forward`.
  * Each step runs touch/key actions through tools/hiby.py while the recorder runs.
  * Detectors (per step):
      flash   a frame that differs a lot from both neighbours while the neighbours are alike
              (a one-frame blink: HiBy content over ByOS, a torn present, a missing layer)
      black   a nearly black frame between two bright ones
      bar     the status bar turning dark for a frame on a bright screen
      stall   a gap of more than 400 ms between changed frames while an animation runs
  * Per step: a contact sheet (PNG) for an independent reviewer, and a JSON report.

Usage
    python tools/ondevice/tour.py [--dev SERIAL] [--out DIR] [--only NAME,NAME]
Needs: adb (platform-tools), Pillow, the player connected (USB ADB recommended: ~5 MB/s).
"""
import argparse, json, os, socket, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from fbrec import decode, png  # noqa: E402

ADB = os.path.expandvars(r"%LOCALAPPDATA%\platform-tools\adb.exe") if os.name == "nt" else "adb"
HIBY = os.path.join(HERE, "..", "hiby.py")
HIBYCTL_LOCAL = os.path.join(HERE, "..", "..", "bin", "hibyctl")
REMOTE = "/usr/data/hibymod/t/hibyctl_fb"
PORT = 5601
DEV = ""
OUT = "."

HOME_UP = "swipe:160,478,160,200,250"
SCROLL = ["swipe:160,380,160,150,300", "wait:1.2", "swipe:160,150,160,380,300", "wait:1.2"]

# name: (actions...). Coordinates are for the default home layout (56 px icons, 4 columns).
TOURS = {
    "home_pages":   ["swipe:280,250,40,250,250", "wait:1.5", "swipe:40,250,280,250,250", "wait:1.5"],
    "settings":     ["tap:53,74", "wait:2.5", *SCROLL, "tap:160,168", "wait:2", "swipe:2,240,250,240,300", "wait:1.5", HOME_UP, "wait:2"],
    "files":        ["tap:124,74", "wait:2.5", *SCROLL, HOME_UP, "wait:2"],
    "radio":        ["tap:195,74", "wait:3", *SCROLL, HOME_UP, "wait:2"],
    "apps":         ["tap:266,74", "wait:3", *SCROLL, HOME_UP, "wait:2"],
    "gutenberg":    ["tap:195,165", "wait:3", *SCROLL, HOME_UP, "wait:2"],
    "vinyl":        ["tap:266,165", "wait:3", *SCROLL, HOME_UP, "wait:2"],
    "books":        ["tap:52,420", "wait:2.5", "tap:160,450", "wait:2", "tap:265,450", "wait:2", HOME_UP, "wait:2"],
    "headphones":   ["tap:124,420", "wait:2.5", *SCROLL, HOME_UP, "wait:2"],
    "nowplaying":   ["tap:196,420", "wait:2.5", "swipe:160,150,160,420,250", "wait:2"],
    "music":        ["tap:268,420", "wait:2.5", *SCROLL, "tap:60,120", "wait:2", "swipe:2,240,250,240,300", "wait:1.5", HOME_UP, "wait:2"],
    "control":      ["swipe:60,5,60,300,250", "wait:2", "tap:274,252", "wait:1.5", "tap:274,252", "wait:1.5", "swipe:160,470,160,150,200", "wait:2"],
    "volume":       ["key:106,event3", "wait:0.3", "key:106,event3", "wait:0.3", "key:105,event3", "wait:0.3", "key:105,event3", "wait:3"],
    "volume_in_app": ["tap:53,74", "wait:2.5", "key:106,event3", "wait:0.3", "key:105,event3", "wait:3", HOME_UP, "wait:2"],
}


def adb(*a, check=False):
    return subprocess.run([ADB, "-s", DEV, *a], capture_output=True, text=True, check=check, env=dict(os.environ, MSYS_NO_PATHCONV="1"))


def start_recorder():
    adb("forward", f"tcp:{PORT}", f"tcp:{PORT}")
    script = (f"chmod 755 {REMOTE}; for p in $(ps | grep 'hibyctl_fb fbrec' | grep -v grep | awk '{{print $1}}'); do kill -9 $p; done; "
              f"sleep 0.3; start-stop-daemon -S -b -x {REMOTE} -- fbrec {PORT} 2 4000")
    adb("shell", script)
    time.sleep(0.8)


def record(path, stop):
    s = None
    for _ in range(40):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=5); break
        except OSError:
            time.sleep(0.2)
    if not s:
        return
    s.settimeout(0.5)
    with open(path, "wb") as f:
        while not stop.is_set():
            try:
                d = s.recv(1 << 17)
            except socket.timeout:
                continue
            if not d:
                break
            f.write(d)
    s.close()


def diff(a, b, step=1):
    return sum(1 for i in range(0, len(a), step) if a[i] != b[i]) * step


def lum(px, y0=0, y1=None, w=160):
    y1 = y1 or len(px) // w
    t = n = 0
    for y in range(y0, y1):
        for x in range(0, w, 4):
            v = px[y * w + x]; t += (v >> 11) * 8 + ((v >> 5) & 63) * 4 + (v & 31) * 8; n += 1
    return t / max(n, 1) / 3


def analyze(frames):
    issues = []
    for i in range(1, len(frames) - 1):
        a, b, c = frames[i - 1]["px"], frames[i]["px"], frames[i + 1]["px"]
        ab = diff(a, b, 3)
        if ab > 1500:
            ac = diff(a, c, 3)
            if ac < ab * 0.12 and frames[i + 1]["t"] - frames[i]["t"] < 120:
                issues.append({"kind": "flash", "t": round(frames[i]["t"]), "px": ab, "dur_ms": round(frames[i + 1]["t"] - frames[i]["t"], 1), "frame": i})
        la, lb, lc = lum(a), lum(b), lum(c)
        if lb < 6 and la > 40 and lc > 40:
            issues.append({"kind": "black", "t": round(frames[i]["t"]), "frame": i})
        sa, sb, sc = lum(a, 0, 14), lum(b, 0, 14), lum(c, 0, 14)
        if sa > 25 and sc > 25 and sb < sa * 0.5 and sb < sc * 0.5:
            issues.append({"kind": "bar", "t": round(frames[i]["t"]), "frame": i})
    for i in range(1, len(frames)):
        g = frames[i]["t"] - frames[i - 1]["t"]
        # a stall inside a burst of changes (both sides animated within 60 ms)
        # ... and the picture jumped across the gap (an animation froze), not a pause between inputs
        if 400 < g < 3000 and i + 1 < len(frames) and i >= 2 and frames[i - 1]["t"] - frames[i - 2]["t"] < 60 and frames[i + 1]["t"] - frames[i]["t"] < 60                 and diff(frames[i - 1]["px"], frames[i]["px"], 3) > 6000:
            issues.append({"kind": "stall", "t": round(frames[i - 1]["t"]), "gap_ms": round(g), "frame": i})
    return issues


def sheet(frames, out, marks):
    from PIL import Image, ImageDraw
    if not frames:
        return
    sel = list(range(len(frames)))
    if len(sel) > 64:                      # keep every issue frame and its neighbours, sample the rest
        keep = {j for m in marks for j in (m - 1, m, m + 1) if 0 <= j < len(frames)}
        step = max(1, len(frames) // 48)
        sel = sorted(keep | set(range(0, len(frames), step)) | {len(frames) - 1})
    W, H, cols = frames[0]["w"], frames[0]["h"], 16
    rows = (len(sel) + cols - 1) // cols
    im = Image.new("RGB", (cols * (W + 2), rows * (H + 14)), (40, 0, 40))
    d = ImageDraw.Draw(im)
    for n, i in enumerate(sel):
        f = frames[i]; t = Image.new("RGB", (W, H))
        t.putdata([((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3) for v in f["px"]])
        x, y = (n % cols) * (W + 2), (n // cols) * (H + 14)
        im.paste(t, (x, y + 12))
        d.text((x + 2, y), f"{f['t']:.0f}", fill=(255, 80, 80) if i in marks else (200, 200, 200))
    im.save(out)


def grab():
    """the frame on the panel right now (half resolution)"""
    p = os.path.join(OUT, "_grab.fbr")
    start_recorder()
    stop = threading.Event(); th = threading.Thread(target=record, args=(p, stop)); th.start()
    time.sleep(0.9); stop.set(); th.join()
    fr = list(decode(p)) if os.path.exists(p) else []
    return fr[-1]["px"] if fr else None


def same_screen(a, b):
    if a is None or b is None:
        return False
    n = len(a); start = 16 * 160                       # without the status bar (clock, battery)
    return sum(1 for i in range(start, n, 5) if a[i] != b[i]) * 5 < (n - start) * 0.03


ESCAPES = [["tap:160,30", "wait:1.5"], [HOME_UP, "wait:2"], ["swipe:280,250,40,250,250", "wait:1.5"],
           ["swipe:160,150,160,420,250", "wait:2"], [HOME_UP, "wait:2"]]


def to_home(ref):
    """every step starts on home page 1: close whatever a previous step left open"""
    env = dict(os.environ, HIBY_DEV=DEV, MSYS_NO_PATHCONV="1")
    for esc in [None, *ESCAPES]:
        if esc:
            subprocess.run([sys.executable, HIBY, *esc], env=env, capture_output=True, text=True)
        if same_screen(grab(), ref):
            return True
    return False


def run_step(name, actions, outdir):
    rec = os.path.join(outdir, name + ".fbr")
    start_recorder()
    stop = threading.Event(); th = threading.Thread(target=record, args=(rec, stop)); th.start()
    time.sleep(0.6)
    env = dict(os.environ, HIBY_DEV=DEV, MSYS_NO_PATHCONV="1")
    t0 = time.time()
    subprocess.run([sys.executable, HIBY, *actions], env=env, capture_output=True, text=True)
    time.sleep(1.0); stop.set(); th.join()
    frames = list(decode(rec)) if os.path.exists(rec) else []
    issues = analyze(frames)
    sheet(frames, os.path.join(outdir, name + ".png"), {x["frame"] for x in issues})
    alive = adb("shell", "pidof hibyshell; pidof hiby_player").stdout.split()
    return {"step": name, "frames": len(frames), "seconds": round(time.time() - t0, 1), "issues": issues,
            "processes": alive, "sheet": name + ".png"}


def main():
    global DEV
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev", default=os.environ.get("HIBY_DEV", "HiBy R3II_2025"))
    ap.add_argument("--out", default=os.path.join(HERE, "..", "..", "out", "tour"))
    ap.add_argument("--only", default="")
    a = ap.parse_args(); DEV = a.dev
    os.makedirs(a.out, exist_ok=True)
    if os.path.exists(HIBYCTL_LOCAL):
        adb("push", HIBYCTL_LOCAL, REMOTE)
    global OUT; OUT = a.out
    names = [n for n in TOURS if not a.only or n in a.only.split(",")]
    pids0 = adb("shell", "pidof hibyshell; pidof hiby_player").stdout.split()
    ref = grab()                       # the tour must start on home page 1 (checked by the user/runner)
    report = []
    for n in names:
        if not to_home(ref):
            print(f"{n:15s} could not get back to the home screen; stopping"); break
        r = run_step(n, TOURS[n], a.out); report.append(r)
        flag = ", ".join(f"{x['kind']}@{x['t']}" for x in r["issues"]) or "ok"
        print(f"{n:15s} {r['frames']:4d} frames  {flag}")
    pids1 = adb("shell", "pidof hibyshell; pidof hiby_player").stdout.split()
    summary = {"steps": report, "pids_before": pids0, "pids_after": pids1,
               "restarted": pids0 != pids1, "log_tail": adb("shell", "tail -40 /tmp/hibymod/shell.log").stdout}
    json.dump(summary, open(os.path.join(a.out, "report.json"), "w"), indent=1)
    print("restarted processes:" if summary["restarted"] else "no process restarted", pids0, pids1)


if __name__ == "__main__":
    main()
