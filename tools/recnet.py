#!/usr/bin/env python3
"""Live screen recording of the player (frame by frame) through ADB.

On the player, `hibyctl recnet 5600 2` sends every presented frame (half resolution, as the
XOR difference with the previous frame, run-length encoded) to whoever connects to port 5600.
Nothing is stored on the player, so recordings can be as long as you like.

    adb forward tcp:5600 tcp:5600
    adb shell "start-stop-daemon -S -b -x /usr/bin/hibyctl -- recnet 5600 2"   # or any copy of hibyctl
    python tools/recnet.py record out.bin        # stops when out.bin.stop exists or with Ctrl+C
    python tools/recnet.py frames out.bin DIR    # writes DIR/NNNNN_<ms>.png
    python tools/recnet.py stats out.bin         # frame count, average and worst interval

Frame format: "SFRM", u32 ms since start, u16 width, u16 height, u32 payload bytes, then
pairs of u16 (repeat count, XOR value).
"""
import array, os, socket, struct, sys, time


def record(out):
    stop = out + ".stop"
    s = None
    for _ in range(50):
        try:
            s = socket.create_connection(("127.0.0.1", 5600), timeout=5); break
        except OSError:
            time.sleep(0.2)
    if not s:
        sys.exit("no connection on 127.0.0.1:5600 (adb forward tcp:5600 tcp:5600?)")
    s.settimeout(2)
    n = total = 0; buf = b""
    try:
        with open(out, "wb") as f:
            while not os.path.exists(stop):
                try:
                    d = s.recv(1 << 16)
                except socket.timeout:
                    continue
                if not d:
                    break
                f.write(d); total += len(d); buf += d
                while len(buf) >= 16 and buf[:4] == b"SFRM":
                    nb = struct.unpack_from("<I", buf, 12)[0]
                    if len(buf) < 16 + nb:
                        break
                    buf = buf[16 + nb:]; n += 1
    except KeyboardInterrupt:
        pass
    print("frames", n, "bytes", total)


def decode(path):
    raw = open(path, "rb").read(); off = 0; prev = None
    while off + 16 <= len(raw) and raw[off:off + 4] == b"SFRM":
        t, wh, nb = struct.unpack_from("<III", raw, off + 4); w, h = wh & 0xFFFF, wh >> 16; off += 16
        if off + nb > len(raw):
            break
        enc = array.array("H"); enc.frombytes(raw[off:off + nb]); off += nb
        cur = array.array("H", [0]) * (w * h) if prev is None else array.array("H", prev)
        i = 0
        for k in range(0, len(enc), 2):
            cnt, x = enc[k], enc[k + 1]
            if x:
                for j in range(i, i + cnt):
                    cur[j] ^= x
            i += cnt
        prev = cur
        yield t, w, h, cur


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == "record":
        record(path)
    elif cmd == "frames":
        from PIL import Image
        d = sys.argv[3]; os.makedirs(d, exist_ok=True)
        for i, (t, w, h, px) in enumerate(decode(path)):
            im = Image.new("RGB", (w, h))
            im.putdata([((p >> 11) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31) for p in px])
            im.save(os.path.join(d, "%05d_%06d.png" % (i, t)))
    elif cmd == "stats":
        ts = [t for t, _, _, _ in decode(path)]
        gaps = [b - a for a, b in zip(ts, ts[1:])]
        if gaps:
            print("frames %d over %d ms, average %.1f ms, worst %d ms, >30 ms: %d"
                  % (len(ts), ts[-1] - ts[0], sum(gaps) / len(gaps), max(gaps), sum(1 for g in gaps if g > 30)))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
