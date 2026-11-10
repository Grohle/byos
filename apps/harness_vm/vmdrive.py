#!/usr/bin/env python3
"""vmdrive.py SCRIPT OUTDIR  - scriptable driver for hibyshell in the QEMU harness (docs/VM_SHELL.md).

Talks to hibyinput in the guest (tcp 127.0.0.1:$HIBY_PORT, default 5566 for ~/hiby/vm_store).
SCRIPT has one command per line ('#' comments):
  mark LABEL            names the next touch (its touch-down) in the report
  tap X Y [MS]          touch down, wait MS (default 60), up
  swipe X1 Y1 X2 Y2 [MS]  drag with a report every ~10 ms, then up
  down X Y | move X Y | up
  drag X1 Y1 X2 Y2 MS   like swipe but WITHOUT lifting (combine with hold/up: pull-to-refresh)
  sleep MS              (host side)
  shot NAME             screenshot of the visible fb -> OUTDIR/NAME.png
  rec N NAME            record the next N presented frames -> share/hs/rec/NAME.bin (tools/recsheet.py)
  sh CMD                run CMD in the guest (output printed)
  restart [nocache]     restart player stand-in + shell (shell_vm.sh)
  wait_log TEXT [MS]    wait until the shell log contains TEXT (new lines only)
At the end it pulls /tmp/hibyinput.log, /tmp/vmplayer.log and the shell log and writes
OUTDIR/report.txt with, for every marked touch:
  feedback  = ms from touch-down (hibyinput, CLOCK_MONOTONIC) to the first presented frame whose
              content changed (hm_vmplayer)  -> touch-to-feedback latency
  frames    = presents between touch-down and 600 ms after release, their mean/max interval
QEMU TCG is not the XBurst: compare numbers relative to each other (before/after), not absolute.
"""
import os
import re
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hibyctl import png_rgb565  # noqa: E402

PORT = int(os.environ.get("HIBY_PORT", "5566"))


class VM:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=600)
        self.f = self.s.makefile("rb")

    def line(self):
        return self.f.readline().decode(errors="replace").rstrip("\n")

    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
        return self.line()

    def sh(self, c):
        self.s.sendall(("sh " + c + "\n").encode())
        out = []
        while True:
            l = self.line()
            if l.startswith(".") and l[1:].lstrip("-").isdigit():
                return "\n".join(out)
            out.append(l)

    def shot(self, path):
        self.s.sendall(b"shot\n")
        hdr = self.line().split()
        if hdr[0] != "shot":
            print("shot failed:", hdr)
            return
        w, h, n = int(hdr[1]), int(hdr[2]), int(hdr[3])
        data = b""
        while len(data) < n:
            data += self.f.read(n - len(data))
        png_rgb565(w, h, data, path)


def run(script, out):
    os.makedirs(out, exist_ok=True)
    vm = VM()
    vm.sh(": > /tmp/hibyinput.log")
    marks = []          # (label, index of the touch-down it names)
    ndown = 0
    pending = None
    log_seen = 0
    for raw in open(script, encoding="utf-8"):
        ln = raw.strip()
        if not ln or ln.startswith("#"):
            continue
        w = ln.split()
        c = w[0]
        if c == "mark":
            pending = " ".join(w[1:])
            continue
        if c in ("tap", "swipe", "down", "drag"):
            if pending:
                marks.append((pending, ndown))
                pending = None
            ndown += 1
        if c == "sleep":
            time.sleep(int(w[1]) / 1000.0)
        elif c == "shot":
            vm.shot(os.path.join(out, w[1] + ".png"))
        elif c == "rec":
            vm.sh("mkdir -p /mnt/share/hs/rec; echo %s /mnt/share/hs/rec/%s.bin > /tmp/vmrec" % (w[1], w[2]))
        elif c == "sh":
            print(vm.sh(ln[3:]))
        elif c == "restart":
            vm.sh("sh /mnt/share/hs/shell_vm.sh restart %s" % (w[1] if len(w) > 1 else ""))
            time.sleep(3)
            log_seen = 0
        elif c == "wait_log":
            text = w[1].replace("_", " ")
            ms = int(w[2]) if len(w) > 2 else 20000
            t0 = time.time()
            while time.time() - t0 < ms / 1000.0:
                lg = vm.sh("cat /tmp/hibymod/shell.log")
                lines = lg.split("\n")
                if any(text in l for l in lines[log_seen:]):
                    log_seen = len(lines)
                    break
                time.sleep(0.3)
            else:
                print("wait_log timeout:", text)
        elif c == "drag":
            x1, y1, x2, y2, ms = map(int, w[1:6])
            steps = max(2, ms // 10)
            for i in range(steps + 1):
                vm.cmd("%s %d %d" % ("down" if i == 0 else "move", x1 + (x2 - x1) * i // steps, y1 + (y2 - y1) * i // steps))
                time.sleep(0.01)
        else:
            r = vm.cmd(ln)
            if r != "ok":
                print(ln, "->", r)
    time.sleep(1.0)
    ev = vm.sh("cat /tmp/hibyinput.log")
    pl = vm.sh("cat /tmp/vmplayer.log")
    sl = vm.sh("cat /tmp/hibymod/shell.log")
    for name in ("hibyinput.log", "vmplayer.log", "shell.log"):
        open(os.path.join(out, name), "w").write({"hibyinput.log": ev, "vmplayer.log": pl, "shell.log": sl}[name])
    report(out, marks, ev, pl, sl)
    vm.s.sendall(b"quit\n")


def report(out, marks, ev, pl, sl):
    downs, ups = [], []
    for l in ev.split("\n"):
        p = l.split()
        if len(p) >= 2 and p[0] == "D":
            downs.append(float(p[1]))
        elif len(p) >= 2 and p[0] == "U":
            ups.append(float(p[1]))
    pres = []
    for l in pl.split("\n"):
        p = l.split()
        if len(p) >= 4 and p[0] == "P":
            pres.append((float(p[1]), int(p[3])))
    lines = ["touch-to-feedback and frame timing (QEMU TCG; relative numbers)", ""]
    lines.append("%-34s %9s %7s %9s %9s" % ("action", "feedback", "frames", "mean ms", "max ms"))
    for label, i in marks:
        if i >= len(downs):
            continue
        td = downs[i]
        tu = next((u for u in ups if u >= td), td)
        fb = next((t - td for t, ch in pres if t > td and ch), None)
        win = [t for t, _ in pres if td < t <= tu + 600]
        iv = [b - a for a, b in zip(win, win[1:])]
        lines.append("%-34s %9s %7d %9s %9s" % (label[:34], "%.1f" % fb if fb is not None else "-", len(win),
                     "%.1f" % (sum(iv) / len(iv)) if iv else "-", "%.1f" % max(iv) if iv else "-"))
    lines += ["", "shell profile lines:"]
    lines += [l for l in sl.split("\n") if re.search(r"tienda|store|perfil", l)]
    txt = "\n".join(lines)
    open(os.path.join(out, "report.txt"), "w").write(txt + "\n")
    print(txt)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    run(sys.argv[1], sys.argv[2])
