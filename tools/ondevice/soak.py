#!/usr/bin/env python3
"""Soak test: runs the on-device tour (tour.py) again and again and records, between passes,
what matters for stability: free memory, process restarts (shell, player), what the stability
guard did, kernel OOM kills and every glitch the tour detected. Writes out/soak/soak_report.json
and keeps each pass's contact sheets in out/soak/passN/.

    python tools/ondevice/soak.py --minutes 40 [--dev SERIAL]
Start it with the player on home page 1. Needs what tour.py needs.
"""
import argparse, json, os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
ADB = os.path.expandvars(r"%LOCALAPPDATA%\platform-tools\adb.exe") if os.name == "nt" else "adb"


def sh(dev, cmd, timeout=30):
    try:
        r = subprocess.run([ADB, "-s", dev, "shell", cmd], capture_output=True, text=True, timeout=timeout,
                           env=dict(os.environ, MSYS_NO_PATHCONV="1"))
        return r.stdout
    except subprocess.TimeoutExpired:
        return None


def sample(dev):
    out = sh(dev, "grep MemAvailable /proc/meminfo; pidof hibyshell; pidof hiby_player; cat /proc/uptime; "
                  "wc -l < /tmp/hibymod/guard.log; dmesg | grep -ci 'killed process'")
    if out is None:
        return {"responsive": False}
    lines = out.split()
    s = {"responsive": True}
    try:
        s["mem_available_kb"] = int(lines[1])
        s["shell_pid"], s["player_pid"] = lines[3], lines[4]
        s["uptime_s"] = float(lines[5])
        s["guard_lines"], s["oom_kills"] = int(lines[7]), int(lines[8])
    except (IndexError, ValueError):
        s["raw"] = out
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev", default=os.environ.get("HIBY_DEV", "HiBy R3II_2025"))
    ap.add_argument("--minutes", type=float, default=40)
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "soak"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    t_end = time.time() + a.minutes * 60
    first = sample(a.dev)
    passes = []
    n = 0
    while time.time() < t_end:
        n += 1
        tdir = os.path.join(a.out, f"pass{n}")
        t0 = time.time()
        r = subprocess.run([sys.executable, os.path.join(HERE, "tour.py"), "--dev", a.dev, "--out", tdir],
                           capture_output=True, text=True, timeout=1800)
        s = sample(a.dev)
        rep = {}
        try:
            rep = json.load(open(os.path.join(tdir, "report.json")))
        except (OSError, ValueError):
            pass
        issues = [dict(step=st["step"], **i) for st in rep.get("steps", []) for i in st["issues"]]
        passes.append({"pass": n, "seconds": round(time.time() - t0), "sample": s, "tour_stdout": r.stdout[-1500:],
                       "issues": issues, "steps": len(rep.get("steps", [])), "restarted": rep.get("restarted")})
        print(f"pass {n}: {len(rep.get('steps', []))} steps, {len(issues)} flags, mem {s.get('mem_available_kb')} KB, "
              f"shell {s.get('shell_pid')} player {s.get('player_pid')}", flush=True)
        json.dump({"first": first, "passes": passes}, open(os.path.join(a.out, "soak_report.json"), "w"), indent=1)
        if not s.get("responsive"):
            print("device not responding; stopping", flush=True)
            break
    guard = sh(a.dev, "cat /tmp/hibymod/guard.log") or ""
    shell_log = sh(a.dev, "grep -E 'activo|salio|CRITICA|rejuven|auto-repar' /tmp/hibymod/shell.log | tail -80") or ""
    json.dump({"first": first, "passes": passes, "guard_log": guard[-6000:], "shell_events": shell_log},
              open(os.path.join(a.out, "soak_report.json"), "w"), indent=1)
    print("done", len(passes), "passes")


if __name__ == "__main__":
    main()
