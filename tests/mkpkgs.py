"""Genera paquetes .hpk de prueba (buenos y maliciosos) para tests/hpk_test.c.
Uso: python3 mkpkgs.py DIR_SALIDA ICON.png
Las cabeceras ustar se escriben a mano para poder fabricar lo que tarfile no deja."""
import os
import struct
import sys

OUT = sys.argv[1]
ICON = open(sys.argv[2], "rb").read()
os.makedirs(OUT, exist_ok=True)


def elf(interp=False, fp=1, machine=8, cls=1, flags=0x70001007):
    """ELF32 minimo: cabecera + PT_LOAD (+ PT_INTERP) (+ PT_MIPS_ABIFLAGS)."""
    phs = [(1, 0, 0x400000, 0x400000, 0x200, 0x200, 5, 0x1000)]
    if interp:
        phs.append((3, 0x100, 0, 0, 20, 20, 4, 1))
    if fp is not None:
        phs.append((0x70000003, 0x140, 0, 0, 24, 24, 4, 8))
    eh = b"\x7fELF" + bytes([cls, 1, 1, 0]) + bytes(8)
    eh += struct.pack("<HHIIIIIHHHHHH", 2, machine, 1, 0x400000, 52, 0, flags, 52, 32, len(phs), 40, 0, 0)
    body = bytearray(eh + b"".join(struct.pack("<8I", *p) for p in phs))
    body += bytes(0x200 - len(body))
    body[0x100:0x114] = b"/lib/ld-musl-mips.so"
    if fp is not None:
        body[0x140:0x158] = struct.pack("<HBBBBBBIIII", 0, 32, 2, 1, 1, 0, fp, 0, 0, 0, 0)
    return bytes(body)


def hdr(name, size=0, typ=b"0", mode=0o644, link="", prefix="", badck=False, raw_size=None):
    h = bytearray(512)
    h[0:len(name.encode())] = name.encode()[:100]
    h[100:108] = b"%07o\0" % mode
    h[108:116] = b"0000000\0"
    h[116:124] = b"0000000\0"
    h[124:136] = raw_size if raw_size is not None else b"%011o\0" % size
    h[136:148] = b"%011o\0" % 946684800
    h[156:157] = typ
    h[157:157 + len(link)] = link.encode()
    h[257:263] = b"ustar\0"
    h[263:265] = b"00"
    h[345:345 + len(prefix)] = prefix.encode()
    h[148:156] = b"        "
    ck = sum(h) + (7 if badck else 0)
    h[148:156] = b"%06o\0 " % ck
    return bytes(h)


def tar(entries, end=True):
    out = b""
    for e in entries:
        if isinstance(e, bytes):           # cabecera ya fabricada (sin datos)
            out += e
            continue
        name, data = e[0], e[1]
        kw = e[2] if len(e) > 2 else {}
        typ = kw.pop("typ", b"0")
        out += hdr(name, len(data) if typ in (b"0", b"\0") else 0, typ, **kw)
        if typ in (b"0", b"\0"):
            out += data + bytes((512 - len(data) % 512) % 512)
    return out + (bytes(1024) if end else b"")


def manifest(id="t.app", version="1.0.0", exec="bin/app", arch="mipsel-hf", min_h="0.1.0", extra=""):
    return ("id=%s\nversion=%s\nmin_hibymod=%s\nname=Prueba\nname.en=Test\nname.fr=Essai\n"
            "description=Una app de prueba\nexec=%s\narch=%s\ninput=touch\nstop_player=1\nram_kb=1234\n%s"
            % (id, version, min_h, exec, arch, extra)).encode()


def good(version="1.0.0", payload=b"v1", big=0, id="t.app", **kw):
    ents = [("manifest.ini", manifest(id=id, version=version, **kw)), ("icon.png", ICON),
            ("bin/", b"", {"typ": b"5", "mode": 0o755}), ("bin/app", elf(), {"mode": 0o755}),
            ("data/info.txt", payload)]
    if big:
        ents.append(("data/big.bin", bytes(range(256)) * (big // 256)))
    return tar(ents)


def w(name, data):
    open(os.path.join(OUT, name), "wb").write(data)


M = ("manifest.ini", manifest())
I = ("icon.png", ICON)
E = ("bin/app", elf(), {"mode": 0o755})
w("good_v1.hpk", good("1.0.0", b"v1"))
w("good_v2.hpk", good("1.1.0", b"v2", big=600 * 1024))
w("good_noicon.hpk", tar([("manifest.ini", manifest(id="t.noicon")), E]))
w("good_dotslash.hpk", tar([("./", b"", {"typ": b"5"}), ("./manifest.ini", manifest(id="t.dot")), ("./bin/app", elf())]))
w("good_prefix.hpk", tar([("manifest.ini", manifest(id="t.prefix")), E,
                          ("deep.txt", b"x", {"prefix": "a" * 60 + "/" + "b" * 60})]))
# --- maliciosos / rotos
w("bad_traversal.hpk", tar([M, I, E, ("../evil.txt", b"pwned")]))
w("bad_traversal2.hpk", tar([M, I, E, ("data/../../evil.txt", b"pwned")]))
w("bad_absolute.hpk", tar([M, I, E, ("/tmp/evil.txt", b"pwned")]))
w("bad_prefix_trav.hpk", tar([M, E, ("evil.txt", b"pwned", {"prefix": ".."})]))
w("bad_backslash.hpk", tar([M, E, ("..\\evil.txt", b"pwned")]))
w("bad_symlink.hpk", tar([M, I, E, hdr("data/link", typ=b"2", link="/etc/passwd")]))
w("bad_symlink_then_file.hpk", tar([M, E, hdr("lnk", typ=b"2", link="/tmp"), ("lnk/evil.txt", b"pwned")]))
w("bad_hardlink.hpk", tar([M, I, E, hdr("data/hl", typ=b"1", link="bin/app")]))
w("bad_chardev.hpk", tar([M, I, E, hdr("data/null", typ=b"3")]))
w("bad_fifo.hpk", tar([M, I, E, hdr("data/fifo", typ=b"6")]))
w("bad_pax.hpk", tar([hdr("PaxHeader", size=0, typ=b"x"), M, I, E]))
bc = bytearray(tar([M, I, E]))
bc[512 + 512 + 60] = ord("X")              # cabecera de icon.png alterada sin recalcular la suma
w("bad_checksum.hpk", bytes(bc))
w("bad_nomanifest.hpk", tar([I, E]))
w("bad_empty.hpk", bytes(1024))
w("bad_notar.hpk", b"PK\x03\x04 esto es un zip" + bytes(2000))
w("bad_huge.hpk", tar([M, I, E]) [:-1024] + hdr("data/huge.bin", raw_size=b"%011o\0" % (100 << 30)) + bytes(4096))
w("bad_toolarge_decl.hpk", tar([M, I, E])[:-1024] + hdr("data/x.bin", raw_size=b"%011o\0" % (300 << 20)) + bytes(1024))
w("bad_base256.hpk", tar([M, E])[:-1024] + hdr("data/b.bin", raw_size=b"\x80" + bytes(10) + b"\x10") + bytes(1024))
w("bad_truncated.hpk", good()[:3000])
w("bad_noexec.hpk", tar([("manifest.ini", manifest(exec="bin/nope")), I, E]))
w("bad_exec_dir.hpk", tar([("manifest.ini", manifest(exec="bin")), I, E]))
w("bad_dynamic.hpk", tar([M, I, ("bin/app", elf(interp=True))]))
w("bad_softfloat.hpk", tar([M, I, ("bin/app", elf(fp=3))]))
w("bad_x86.hpk", tar([M, I, ("bin/app", elf(machine=3))]))
w("bad_elf64.hpk", tar([M, I, ("bin/app", elf(cls=2))]))
w("bad_mips64isa.hpk", tar([M, I, ("bin/app", elf(flags=0x80000000))]))
w("bad_script.hpk", tar([M, I, ("bin/app", b"#!/bin/sh\necho hi\n")]))
w("bad_id.hpk", tar([("manifest.ini", manifest(id="../x")), I, E]))
w("bad_id_upper.hpk", tar([("manifest.ini", manifest(id="Org.App")), I, E]))
w("bad_id_long.hpk", tar([("manifest.ini", manifest(id="a" * 49)), I, E]))
w("bad_version.hpk", tar([("manifest.ini", manifest(version="1.0")), I, E]))
w("bad_arch.hpk", tar([("manifest.ini", manifest(arch="mipsel-sf")), I, E]))
w("bad_newer.hpk", tar([("manifest.ini", manifest(min_h="9.0.0")), I, E]))
w("bad_exec_trav.hpk", tar([("manifest.ini", manifest(exec="../bin/app")), I, E]))
w("bad_input.hpk", tar([("manifest.ini", manifest().replace(b"input=touch", b"input=mouse")), I, E]))
w("bad_noname.hpk", tar([("manifest.ini", manifest().replace(b"name=Prueba\n", b"")), I, E]))
w("bad_dup.hpk", tar([M, I, E, ("data/a.txt", b"1"), ("data/a.txt", b"2")]))
w("bad_dupmanifest.hpk", tar([M, M, I, E]))
w("bad_file_dir.hpk", tar([M, I, E, ("data", b"file"), ("data/x.txt", b"2")]))
w("bad_icon.hpk", tar([M, ("icon.png", b"\x89PNG\r\n\x1a\nbasura" + bytes(100)), E]))
w("bad_icon_huge.hpk", tar([M, ("icon.png", bytes(2 << 20)), E]))
w("bad_manifest_big.hpk", tar([("manifest.ini", manifest() + b"#" * 20000), I, E]))
print("paquetes en", OUT)
