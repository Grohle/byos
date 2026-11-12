"""Client for `hibyctl fbrec PORT SCALE PERIOD_US` (FBR2 stream).
  python fbrec.py record OUT SECONDS [port]
  python fbrec.py analyze OUT DIR      -> flicker report + PNGs of suspicious frames (+ neighbours)
"""
import array, os, socket, struct, sys, time, zlib


def record(out, secs, port=5601):
    s = None
    for _ in range(50):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=5); break
        except OSError:
            time.sleep(0.2)
    if not s:
        sys.exit("no connection")
    s.settimeout(1)
    t_end = time.time() + secs; total = 0
    with open(out, "wb") as f:
        while time.time() < t_end:
            try:
                d = s.recv(1 << 17)
            except socket.timeout:
                continue
            if not d:
                break
            f.write(d); total += len(d)
    s.close(); print("bytes", total)


def decode(path):
    raw = open(path, "rb").read(); off = 0; prev = None
    while off + 32 <= len(raw) and raw[off:off + 4] == b"FBR2":
        t, = struct.unpack_from("<Q", raw, off + 4)
        w, h, yo, same = struct.unpack_from("<HHHH", raw, off + 12)
        gap, nb, rd = struct.unpack_from("<III", raw, off + 20)
        off += 32
        if off + nb > len(raw):
            break
        enc = array.array("H"); enc.frombytes(raw[off:off + nb]); off += nb
        cur = array.array("H", [0]) * (w * h) if prev is None else array.array("H", prev)
        i = 0
        for k in range(0, len(enc), 2):
            n, d = enc[k], enc[k + 1]
            if d:
                for j in range(i, i + n):
                    cur[j] ^= d
            i += n
        prev = cur
        yield dict(t=t / 1000.0, w=w, h=h, yo=yo, same=same, gap=gap, rd=rd, px=cur)


def png(path, w, h, px):
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        for x in range(w):
            v = px[y * w + x]
            rows += bytes(((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3))
    def ch(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + ch(b"IDAT", zlib.compress(bytes(rows), 6)) + ch(b"IEND", b""))


def diff(a, b):
    n = 0; rows = set()
    w = 160
    for i in range(0, len(a)):
        if a[i] != b[i]:
            n += 1; rows.add(i // w)
    return n, (min(rows), max(rows)) if rows else None


def analyze(path, outdir):
    os.makedirs(outdir, exist_ok=True)
    fr = list(decode(path))
    print("frames", len(fr), "span %.1f s" % ((fr[-1]["t"] - fr[0]["t"]) / 1000 if fr else 0))
    if not fr:
        return
    ts = [f["t"] for f in fr]
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    print("changed frames/s %.1f" % (len(fr) / max(1e-3, (ts[-1] - ts[0]) / 1000)))
    print("yoffsets", sorted(set(f["yo"] for f in fr)))
    flick = []
    for i in range(1, len(fr) - 1):
        a, b, c = fr[i - 1]["px"], fr[i]["px"], fr[i + 1]["px"]
        nac, _ = diff(a, c)
        nab, rr = diff(a, b)
        if nab > 200 and nac < nab * 0.1:
            flick.append((i, nab, nac, rr, fr[i]["t"] - fr[i - 1]["t"], fr[i + 1]["t"] - fr[i]["t"]))
    print("flash frames (differs from both neighbours, neighbours alike):", len(flick))
    for i, nab, nac, rr, d1, d2 in flick[:60]:
        print("  #%d t=%.0fms changed=%d px rows=%s neighbours_diff=%d dur=%.1fms yo=%d" % (i, fr[i]["t"], nab, rr, nac, d2, fr[i]["yo"]))
    for i, *_ in flick[:12]:
        for j in (i - 1, i, i + 1):
            png(os.path.join(outdir, "%05d_%07.0f.png" % (j, fr[j]["t"])), fr[j]["w"], fr[j]["h"], fr[j]["px"])
    # periodic report of frame-to-frame changes
    big = [(fr[i]["t"], diff(fr[i - 1]["px"], fr[i]["px"])) for i in range(1, len(fr))]
    print("first 40 changes (t ms, changed px, rows):")
    for t, (n, rr) in big[:40]:
        print("  %.0f %d %s" % (t, n, rr))
    png(os.path.join(outdir, "first.png"), fr[0]["w"], fr[0]["h"], fr[0]["px"])


if __name__ == "__main__":
    if sys.argv[1] == "record":
        record(sys.argv[2], float(sys.argv[3]), int(sys.argv[4]) if len(sys.argv) > 4 else 5601)
    else:
        analyze(sys.argv[2], sys.argv[3])
