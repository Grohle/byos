#!/usr/bin/env python3
"""
Edits to HiBy's own files, stored WITHOUT HiBy's text.

HiByOS changes a few files that belong to the official firmware (two start-up scripts and
some screen layouts). Those files are never stored in this repository or in anything built
from it. tools/vendor_edits.json holds, for each of them, a list of operations that rebuilds
the HiByOS version from the copy inside the user's own firmware:

    {"copy": [offset, length]}   take `length` bytes at `offset` of the official file
    {"add":  ["line", "line"]}   insert text written for HiByOS (the lines joined with "\\n")

so the only text in the data file is ours. tools/build.py and tools/hibyos_patcher.py apply
the list while they patch the firmware; tools/make_payload.py compiles the same kind of list
for the files that the installer derives on the player (payload/derive/, docs/INSTALLER.md).

Format of tools/vendor_edits.json (version 1), precise enough to re-implement:

    {"format": 1,
     "files": [
       {"path": "usr/bin/hiby_player.sh",            # in the root filesystem, no leading "/"
        "stock":  {"size": N, "sha256": "..."},      # the official file, as found in the firmware
        "strip_cr": false,                           # true: delete every 0x0D byte of the
                                                     #   official file BEFORE applying the ops
        "result": {"size": N, "sha256": "..."},      # the file HiByOS installs
        "ops": [ {"copy": [off, len]} | {"add": [str, ...]} , ... ],
        "previous": [ {"result": {...}, "ops": [...]}, ... ]   # optional, see below
       }, ...]}

To apply an entry:
  1. read the official file; its size and SHA-256 must be `stock` (otherwise stop: unknown
     firmware, nothing is written);
  2. base = those bytes, minus every 0x0D byte when `strip_cr` is true;
  3. result = concatenation, in order, of base[off:off+len] for each "copy" (0 <= off,
     len > 0, off + len <= len(base)) and of the UTF-8 bytes of "\\n".join(lines) for each "add";
  4. the size and SHA-256 of the result must be `result` (otherwise stop);
  5. the result replaces the file; mode and owner of the official file are kept.

"previous" lists older HiByOS versions of the same file (same ops format, same base). The
firmware builders ignore it. make_payload.py uses it so that a new payload can also update a
player whose firmware already carries one of those older versions (the pristine official file
is no longer on such a player).

Developer commands (they need YOUR unpacked copy of the official root filesystem):

    python tools/vendor_edits.py check  --stock ROOTFS          verify every entry, show numbers
    python tools/vendor_edits.py export --stock ROOTFS [--out DIR]
                         write the full HiByOS versions to DIR (default out/vendor_files,
                         not tracked by git) so they can be read and edited
    python tools/vendor_edits.py update --stock ROOTFS [--from DIR] [--keep-previous]
                         rebuild the entries from the full files in DIR

Never commit the exported files: they contain HiBy's text.
"""
import argparse
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
JSON_PATH = os.path.join(getattr(sys, "_MEIPASS", HERE), "vendor_edits.json")
if not os.path.isfile(JSON_PATH):
    JSON_PATH = os.path.join(HERE, "vendor_edits.json")
FORMAT = 1
# Generator: any run of at least this many bytes that also exists in the official file is
# referenced with "copy", never stored. Scripts use a longer minimum so that our own lines stay
# readable in the data file (they are almost entirely ours, and a short coincidence such as
# "/usr/bin/" is not worth a reference).
MIN_MATCH = 8
MIN_MATCH_SCRIPT = 16
EXPORT_DIR = os.path.join(HERE, "..", "out", "vendor_files")


class EditError(Exception):
    pass


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def ident(b):
    return {"size": len(b), "sha256": sha256(b)}


# ------------------------------------------------------------------ applying (runtime)
def load(path=None):
    """The list of entries of vendor_edits.json, validated."""
    with open(path or JSON_PATH, encoding="utf-8") as f:
        doc = json.load(f)
    if doc.get("format") != FORMAT:
        raise EditError(f"vendor_edits.json: unsupported format {doc.get('format')!r}")
    seen = set()
    for e in doc["files"]:
        p = e["path"]
        if p in seen or p.startswith("/") or "\\" in p or ".." in p.split("/"):
            raise EditError(f"vendor_edits.json: bad or repeated path {p!r}")
        seen.add(p)
        for key in ("stock", "result"):
            if not (isinstance(e[key]["size"], int) and len(e[key]["sha256"]) == 64):
                raise EditError(f"vendor_edits.json: {p}: bad {key!r}")
        if not isinstance(e.get("strip_cr", False), bool) or not isinstance(e["ops"], list):
            raise EditError(f"vendor_edits.json: {p}: bad entry")
    return doc["files"]


def base_of(entry, stock_bytes):
    """Steps 1 and 2: check the official file and return the bytes the ops refer to."""
    if len(stock_bytes) != entry["stock"]["size"] or sha256(stock_bytes) != entry["stock"]["sha256"]:
        raise EditError(f"{entry['path']}: this is not the expected official file "
                        f"(size {len(stock_bytes)}, sha256 {sha256(stock_bytes)})")
    return stock_bytes.replace(b"\r", b"") if entry.get("strip_cr") else stock_bytes


def run_ops(ops, base, what="?"):
    """Step 3."""
    out = []
    for op in ops:
        if len(op) != 1:
            raise EditError(f"{what}: bad operation {op!r}")
        (kind, arg), = op.items()
        if kind == "copy":
            off, n = arg
            if not (isinstance(off, int) and isinstance(n, int) and 0 <= off and n > 0 and off + n <= len(base)):
                raise EditError(f"{what}: copy [{off}, {n}] is outside the official file")
            out.append(base[off:off + n])
        elif kind == "add":
            out.append("\n".join(arg).encode("utf-8"))
        else:
            raise EditError(f"{what}: unknown operation {kind!r}")
    return b"".join(out)


def apply(entry, stock_bytes, version=None):
    """The HiByOS version of the file (steps 1 to 4). `version`: an item of "previous"."""
    v = version or entry
    res = run_ops(v["ops"], base_of(entry, stock_bytes), entry["path"])
    if len(res) != v["result"]["size"] or sha256(res) != v["result"]["sha256"]:
        raise EditError(f"{entry['path']}: the edited file is not the expected one")
    return res


# ------------------------------------------------------------------ generating (developer)
def make_ops(base, result, min_match=MIN_MATCH):
    """Greedy delta: at every position of `result` the longest run also present in `base` is
    referenced when it is at least `min_match` bytes long; everything else is literal ("add").
    Consequence: no `min_match`-byte window that starts inside a literal exists in `base`."""
    m = min_match
    idx = {}
    for i in range(len(base) - m + 1):
        idx.setdefault(base[i:i + m], []).append(i)
    ops, lit, i, n = [], bytearray(), 0, len(result)

    def flush():
        if lit:
            ops.append({"add": bytes(lit).decode("utf-8").split("\n")})
            lit.clear()

    while i < n:
        best_len, best_pos = 0, -1
        for p in idx.get(result[i:i + m], ()):
            k = m
            while p + k < len(base) and i + k < n and base[p + k] == result[i + k]:
                k += 1
            if k > best_len:
                best_len, best_pos = k, p
        if best_len >= m:
            flush()
            ops.append({"copy": [best_pos, best_len]})
            i += best_len
        else:
            lit.append(result[i])
            i += 1
    flush()
    return ops


def make_version(base, result, min_match=MIN_MATCH):
    ops = make_ops(base, result, min_match)
    if run_ops(ops, base) != result:
        raise EditError("internal error: the generated operations do not rebuild the file")
    return {"result": ident(result), "ops": ops}


def min_match_for(result):
    return MIN_MATCH_SCRIPT if result.startswith(b"#!") else MIN_MATCH


def make_entry(path, stock_bytes, result, min_match=None):
    min_match = min_match or min_match_for(result)
    strip = b"\r" in stock_bytes and b"\r" not in result
    base = stock_bytes.replace(b"\r", b"") if strip else stock_bytes
    v = make_version(base, result, min_match)
    return {"path": path, "stock": ident(stock_bytes), "strip_cr": strip,
            "result": v["result"], "ops": v["ops"]}


def literals(ops):
    return ["\n".join(a).encode("utf-8") for op in ops for k, a in op.items() if k == "add"]


def longest_shared(lits, base):
    """Length of the longest run of literal bytes that also exists in the official file."""
    best = 0
    for lit in lits:
        i = 0
        while i < len(lit):
            k = best + 1
            if i + k > len(lit):
                break
            if lit[i:i + k] in base:
                while i + k < len(lit) and lit[i:i + k + 1] in base:
                    k += 1
                best = k
            else:
                i += 1
    return best


def numbers(ops, base):
    cp = sum(a[1] for op in ops for k, a in op.items() if k == "copy")
    lits = literals(ops)
    return {"ops": len(ops), "copied": cp, "added": sum(len(x) for x in lits),
            "longest_shared": longest_shared(lits, base)}


def _dump_ops(ops, ind):
    out = []
    for op in ops:
        (kind, arg), = op.items()
        if kind == "copy" or len(arg) == 1:
            out.append(ind + json.dumps(op, ensure_ascii=False))
        else:
            body = ",\n".join(ind + "  " + json.dumps(s, ensure_ascii=False) for s in arg)
            out.append(ind + '{"add": [\n' + body + "\n" + ind + "]}")
    return ",\n".join(out)


def dump(entries, path=None):
    """Writes vendor_edits.json: plain JSON, one operation (or one added line) per text line
    so that a change to our text shows up as a normal line diff."""
    parts = []
    for e in entries:
        s = ("  {\n"
             f'   "path": {json.dumps(e["path"])},\n'
             f'   "stock": {json.dumps(e["stock"])},\n'
             f'   "strip_cr": {json.dumps(bool(e.get("strip_cr")))},\n'
             f'   "result": {json.dumps(e["result"])},\n'
             '   "ops": [\n' + _dump_ops(e["ops"], "    ") + "\n   ]")
        if e.get("previous"):
            prev = []
            for v in e["previous"]:
                prev.append("    {\n"
                            f'     "result": {json.dumps(v["result"])},\n'
                            '     "ops": [\n' + _dump_ops(v["ops"], "      ") + "\n     ]\n    }")
            s += ',\n   "previous": [\n' + ",\n".join(prev) + "\n   ]"
        parts.append(s + "\n  }")
    text = ('{\n "format": 1,\n'
            ' "about": "Edits to files of the official HiBy firmware, without their text. '
            'See tools/vendor_edits.py for the format.",\n'
            ' "files": [\n' + ",\n".join(parts) + "\n ]\n}\n")
    json.loads(text)
    with open(path or JSON_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def record(path, stock_bytes, result, keep_previous=False, json_path=None, min_match=None):
    """Create or replace the entry for `path` (used by the theme generators). Returns True when
    the entry changed. With keep_previous the replaced version is kept in "previous"."""
    jp = json_path or JSON_PATH
    entries = load(jp) if os.path.isfile(jp) else []
    new = make_entry(path, stock_bytes, result, min_match)
    for i, e in enumerate(entries):
        if e["path"] != path:
            continue
        if e["stock"] != new["stock"]:
            raise EditError(f"{path}: the official file differs from the one the entry was made for")
        if e["result"] == new["result"]:
            return False
        prev = [v for v in e.get("previous", []) if v["result"] != new["result"]]
        if keep_previous:
            prev.insert(0, {"result": e["result"], "ops": e["ops"]})
        if prev:
            new["previous"] = prev
        entries[i] = new
        break
    else:
        entries.append(new)
        entries.sort(key=lambda e: e["path"])
    dump(entries, jp)
    return True


# ------------------------------------------------------------------ command line
def _stock(root, rel):
    p = os.path.join(root, *rel.split("/"))
    if not os.path.isfile(p):
        raise EditError(f"{rel}: not found under {root} (is this the unpacked official root filesystem?)")
    with open(p, "rb") as f:
        return f.read()


def cmd_check(a):
    entries = load(a.json)
    print(f"{len(entries)} files in {a.json or JSON_PATH}")
    print(f"{'file':58s} {'stock':>6s} {'result':>6s} {'ops':>4s} {'copied':>6s} {'added':>6s} {'shared':>6s}")
    worst = 0
    for e in entries:
        st = _stock(a.stock, e["path"])
        base = base_of(e, st)
        for v in [e] + e.get("previous", []):
            apply(e, st, v)
            nb = numbers(v["ops"], base)
            worst = max(worst, nb["longest_shared"])
            name = e["path"] if v is e else "   previous " + v["result"]["sha256"][:12]
            print(f"{name:58s} {len(st):6d} {v['result']['size']:6d} {nb['ops']:4d} {nb['copied']:6d} "
                  f"{nb['added']:6d} {nb['longest_shared']:6d}")
    print("OK: every entry rebuilds its file from the official one.")
    print(f"'shared' = longest run of stored text that also exists in the official file: {worst} bytes at most.")
    return 0


def cmd_export(a):
    out = os.path.abspath(a.out or EXPORT_DIR)
    for e in load(a.json):
        data = apply(e, _stock(a.stock, e["path"]))
        p = os.path.join(out, *e["path"].split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
        print("  ", e["path"])
    print(f"Full files written to {out}\nThey contain HiBy's text: do not commit or publish them.")
    return 0


def cmd_update(a):
    src = os.path.abspath(a.src or EXPORT_DIR)
    n = 0
    for e in load(a.json):
        p = os.path.join(src, *e["path"].split("/"))
        if not os.path.isfile(p):
            continue
        with open(p, "rb") as f:
            data = f.read().replace(b"\r\n", b"\n")
        if record(e["path"], _stock(a.stock, e["path"]), data, a.keep_previous, a.json):
            print("  updated:", e["path"])
            n += 1
    for rel in a.add or []:
        p = os.path.join(src, *rel.split("/"))
        with open(p, "rb") as f:
            data = f.read().replace(b"\r\n", b"\n")
        if record(rel, _stock(a.stock, rel), data, False, a.json):
            print("  added:", rel)
            n += 1
    print(f"{n} entries changed")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Edits to HiBy's files stored without HiBy's text.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("check", "export", "update"):
        p = sub.add_parser(name)
        p.add_argument("--stock", required=True, help="your unpacked official root filesystem")
        p.add_argument("--json", help=argparse.SUPPRESS)
        if name == "export":
            p.add_argument("--out", help="folder for the full files (default: out/vendor_files)")
        if name == "update":
            p.add_argument("--from", dest="src", help="folder with the full files (default: out/vendor_files)")
            p.add_argument("--keep-previous", action="store_true",
                           help="keep the replaced version in \"previous\" (do this for versions "
                                "that were released in a firmware)")
            p.add_argument("--add", action="append", metavar="PATH",
                           help="also create an entry for this path (a new edited file)")
    a = ap.parse_args(argv)
    try:
        return {"check": cmd_check, "export": cmd_export, "update": cmd_update}[a.cmd](a)
    except EditError as e:
        print("ERROR:", e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
