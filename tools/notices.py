#!/usr/bin/env python3
"""
Third-party notices for the files ByOS distributes.

Every distributed file set (an "artefact") must carry THIRD_PARTY_NOTICES.md and the
licence texts of the third-party components it contains. This module knows which licence
files each artefact needs and copies them where a build script asks.

Command line:

  python tools/notices.py --list                 show the artefacts and their files
  python tools/notices.py --check                verify that every listed file exists
  python tools/notices.py <artefact> <dest_dir>  copy the notices of <artefact> into <dest_dir>

From a build script:

  import notices
  written = notices.collect("sd-payload", os.path.join(payload, "licenses"))

The component list, with versions and copyright notices, is THIRD_PARTY_NOTICES.md at the
root of the repository. When a component is added to or removed from an artefact, update
that document, LICENSES/ and the ARTEFACTS table below together.

No dependencies outside the Python standard library.
"""
import os
import shutil
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The notices document; collect() always copies it first.
NOTICES = "THIRD_PARTY_NOTICES.md"

# Linked into every MIPS program built with "zig cc": musl libc and the Zig runtime
# (compiler-rt, parts of which were ported from musl and from LLVM).
_MIPS_RUNTIME = [
    "LICENSES/musl-COPYRIGHT.txt",
    "LICENSES/Zig-LICENSE.txt",
    "LICENSES/LLVM-LICENSE.txt",
]

# overlay/: hibyboot (stb_image), hibyctl, wpa_cli; images drawn with the Inter typeface.
_FIRMWARE = _MIPS_RUNTIME + [
    "LICENSES/stb-LICENSE.txt",
    "LICENSES/OFL-1.1-Inter.txt",
]

# artefact name -> licence files it needs (paths relative to the repository root).
# collect() copies NOTICES plus exactly these files.
ARTEFACTS = {
    # The files added to the firmware image (the overlay/ tree).
    "firmware": _FIRMWARE,

    # out/sd/ByOS/payload: hibyshell, hibywall, hibymix, hibytag, hibynet,
    # hibymod_disp.so and cacert.pem.
    "sd-payload": _MIPS_RUNTIME + [
        "LICENSES/stb-LICENSE.txt",              # hibyshell, hibywall, hibynet
        "LICENSES/SQLite-blessing.txt",          # hibymix, hibytag
        "LICENSES/IJG-libjpeg-README.txt",       # hibywall, hibynet
        "LICENSES/MbedTLS-LICENSE.txt",          # hibynet
        "LICENSES/Apache-2.0.txt",               # Mbed TLS
        "LICENSES/MPL-2.0.txt",                  # cacert.pem (Mozilla CA list)
        "LICENSES/TweetNaCl-public-domain.txt",  # hibyshell (Ed25519 verification)
        "LICENSES/glibc-nonshared-NOTICE.txt",   # hibymod_disp.so
        "LICENSES/glibc-LICENSES.txt",           # hibymod_disp.so
        "LICENSES/LGPL-2.1.txt",                 # glibc pieces in hibymod_disp.so
        "LICENSES/OFL-1.1-Inter.txt",            # digits and images drawn with Inter
    ],

    # ByOS-Patcher-<version>-python.zip: Python scripts, vendor/pycdlib and a copy of overlay/.
    "patcher-python": _FIRMWARE + [
        "LICENSES/pycdlib-COPYING.txt",
    ],

    # ByOS-Patcher.exe (PyInstaller one-file program) and the zip that contains it:
    # Python runtime, PyInstaller bootloader, pycdlib and a copy of overlay/.
    "patcher-windows": _FIRMWARE + [
        "LICENSES/pycdlib-COPYING.txt",
        "LICENSES/Python-LICENSE.txt",
        "LICENSES/Python-incorporated-software.rst",
        "LICENSES/PyInstaller-COPYING.txt",
        "LICENSES/GPL-2.0.txt",                  # PyInstaller bootloader (with its exception)
        "LICENSES/Apache-2.0.txt",               # OpenSSL, PyInstaller run-time hooks
        "LICENSES/zlib-LICENSE.txt",             # zlib inside the PyInstaller bootloader
    ],

    # Browser app: bin/nsfb (NetSurf and its libraries), bin/navegador, res/, ca.pem.
    "hpk:org.hibymod.navegador": _MIPS_RUNTIME + [
        "LICENSES/NetSurf-COPYING.txt",
        "LICENSES/GPL-2.0.txt",                  # NetSurf; Mbed TLS as linked into NetSurf
        "LICENSES/NetSurf-libraries-COPYING.txt",
        "LICENSES/utf8proc-LICENSE.md",
        "LICENSES/talloc-NOTICE.txt",
        "LICENSES/LGPL-2.0.txt",                 # talloc
        "LICENSES/LGPL-2.1.txt",                 # talloc
        "LICENSES/punycode-RFC3492.txt",
        "LICENSES/MPL-2.0.txt",                  # Public Suffix List, ca.pem
        "LICENSES/MbedTLS-LICENSE.txt",
        "LICENSES/curl-COPYING.txt",
        "LICENSES/zlib-LICENSE.txt",
        "LICENSES/libpng-LICENSE.txt",
        "LICENSES/expat-COPYING.txt",
        "LICENSES/IJG-libjpeg-README.txt",
    ],

    # byos-cli.jar (desktop app) and the ByOS Install app's core (THIRD_PARTY_NOTICES section 7).
    "byos-cli": [
        "LICENSES/hiby-mods-LICENSE.txt",        # database rules, re-implemented in Kotlin
        "LICENSES/TweetNaCl-public-domain.txt",  # X25519, re-implemented
        "LICENSES/Apache-2.0.txt",               # Kotlin standard library, JetBrains annotations
    ],
    "android-install": [
        "LICENSES/hiby-mods-LICENSE.txt",
        "LICENSES/TweetNaCl-public-domain.txt",
    ],

    # Hello sample app: bin/hola with the Terminus bitmap font.
    "hpk:org.hibymod.hola": _MIPS_RUNTIME + [
        "LICENSES/OFL-1.1-Terminus.txt",
    ],
}


def files_for(artefact):
    """Return the files of `artefact` (relative to the repository root), notices document first."""
    if artefact not in ARTEFACTS:
        raise ValueError("unknown artefact %r (known: %s)" % (artefact, ", ".join(sorted(ARTEFACTS))))
    out = [NOTICES]
    for rel in ARTEFACTS[artefact]:
        if rel not in out:
            out.append(rel)
    return out


def collect(artefact, dest_dir):
    """Copy THIRD_PARTY_NOTICES.md and the licence files of `artefact` into `dest_dir`.

    The files are copied flat (no sub-folders), byte for byte, and `dest_dir` is created if
    needed. Returns the list of files written (absolute paths, notices document first).
    Raises ValueError for an unknown artefact and FileNotFoundError if a source file is
    missing; in both cases nothing is written.
    """
    rels = files_for(artefact)
    pairs, seen = [], {}
    for rel in rels:
        src = os.path.join(REPO, *rel.split("/"))
        if not os.path.isfile(src):
            raise FileNotFoundError("notices file missing for %s: %s" % (artefact, src))
        name = os.path.basename(src)
        if name.lower() in seen:
            raise ValueError("two notices files of %s share the name %s" % (artefact, name))
        seen[name.lower()] = rel
        pairs.append((src, name))
    dest_dir = os.path.abspath(dest_dir)
    os.makedirs(dest_dir, exist_ok=True)
    written = []
    for src, name in pairs:
        dst = os.path.join(dest_dir, name)
        shutil.copyfile(src, dst)
        written.append(dst)
    return written


def check():
    """Return a list of problems (empty when the table and the LICENSES/ folder agree)."""
    problems = []
    used = set()
    for artefact in sorted(ARTEFACTS):
        names = set()
        for rel in files_for(artefact):
            used.add(rel)
            if not os.path.isfile(os.path.join(REPO, *rel.split("/"))):
                problems.append("%s: missing file %s" % (artefact, rel))
            base = os.path.basename(rel).lower()
            if base in names:
                problems.append("%s: duplicate file name %s" % (artefact, base))
            names.add(base)
    lic = os.path.join(REPO, "LICENSES")
    if os.path.isdir(lic):
        for name in sorted(os.listdir(lic)):
            rel = "LICENSES/" + name
            if name != "README.md" and rel not in used:
                problems.append("LICENSES/%s is not used by any artefact" % name)
    else:
        problems.append("the LICENSES folder is missing")
    return problems


def main(argv):
    if argv == ["--list"]:
        for artefact in ARTEFACTS:
            print(artefact)
            for rel in files_for(artefact):
                print("   ", rel)
        return 0
    if argv == ["--check"]:
        problems = check()
        for p in problems:
            print("ERROR:", p)
        if not problems:
            print("OK: %d artefacts, every listed file exists" % len(ARTEFACTS))
        return 1 if problems else 0
    if len(argv) != 2 or argv[0].startswith("-"):
        sys.stderr.write(__doc__)
        return 2
    try:
        written = collect(argv[0], argv[1])
    except (ValueError, FileNotFoundError) as e:
        sys.stderr.write("ERROR: %s\n" % e)
        return 1
    for path in written:
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
