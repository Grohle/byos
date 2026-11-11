# fetch.sh - shared helpers for the ByOS build scripts. Source this file; do not run it.
#
#     . "$(dirname "$0")/fetch.sh"
#
# Every third-party input of the build is pinned: an exact version, an exact URL and the SHA-256
# of the file. Nothing downloaded is used before its hash has been checked.
#
#   fetch_verify URL SHA256 DEST
#       Makes sure that DEST exists and has exactly that SHA-256.
#         - DEST is already there: its hash is checked and nothing is downloaded.
#         - DEST is missing (or empty): URL is downloaded to a temporary file next to DEST,
#           the hash is checked, and only then is the file moved into place.
#       On any failure it prints an error and returns 1. The build scripts run with "set -e",
#       so they stop there. A file with the wrong hash is never left at DEST by this function.
#
#   verify_sha256 FILE SHA256
#       Checks a file that is already on disk (for example one unpacked from a pinned archive).
#
#   zig_check [PYTHON]
#       Checks that the cross compiler is the pinned one (see "Pinned toolchain" below).
#
# ---------------------------------------------------------------------------------------------
# Pinned toolchain
#
#   All device binaries are cross-compiled with "zig cc" from the PyPI package "ziglang".
#   Pinned version: ziglang 0.17.1  (clang/LLVM 22.1.8, bundled musl libc 1.2.5).
#   No script installs it; install it once, by hand, with the exact version:
#
#       python3 -m venv ~/hiby/venv
#       ~/hiby/venv/bin/pip install ziglang==0.17.1
#
#   The wheel used for the published binaries (Linux x86_64) is
#       ziglang-0.17.1-py3-none-manylinux_2_12_x86_64.manylinux2010_x86_64.musllinux_1_1_x86_64.whl
#       SHA-256 ea6cc5d87b033cf0aedfbc8f9549a908cae6c50ac31f1698188fa48a74524b54
#   To make pip check that hash as well (Linux x86_64 only; other platforms have other wheels):
#
#       echo "ziglang==0.17.1 --hash=sha256:ea6cc5d87b033cf0aedfbc8f9549a908cae6c50ac31f1698188fa48a74524b54" > zig-req.txt
#       ~/hiby/venv/bin/pip install --require-hashes -r zig-req.txt
#
#   A different Zig version produces different binaries. zig_check stops the build when the
#   version is not the pinned one. To build with another version on purpose, set
#   HIBYOS_ALLOW_OTHER_ZIG=1 (the check then only prints a warning).
# ---------------------------------------------------------------------------------------------

HIBYOS_ZIG_VERSION=0.17.1

verify_sha256() {
    if [ $# -ne 2 ]; then echo "usage: verify_sha256 FILE SHA256" >&2; return 2; fi
    if [ ! -f "$1" ]; then
        echo "ERROR: $1 is missing (expected a file with SHA-256 $2)." >&2
        return 1
    fi
    _vs_got=$(sha256sum "$1" | cut -d' ' -f1)
    if [ "$_vs_got" != "$2" ]; then
        echo "ERROR: SHA-256 mismatch for $1" >&2
        echo "  expected: $2" >&2
        echo "  found:    $_vs_got" >&2
        return 1
    fi
    return 0
}

fetch_verify() {
    if [ $# -ne 3 ]; then echo "usage: fetch_verify URL SHA256 DEST" >&2; return 2; fi
    _fv_url=$1; _fv_want=$2; _fv_dest=$3
    if [ -s "$_fv_dest" ]; then
        if verify_sha256 "$_fv_dest" "$_fv_want"; then
            echo "verified: $_fv_dest (already present, SHA-256 matches)"
            return 0
        fi
        echo "  This existing file is not the pinned version and was left untouched." >&2
        echo "  Delete it and run this script again to download the pinned copy from" >&2
        echo "  $_fv_url" >&2
        return 1
    fi
    _fv_tmp="$_fv_dest.part.$$"
    echo "downloading: $_fv_url"
    if ! curl -sSfL --retry 3 -o "$_fv_tmp" "$_fv_url"; then
        rm -f "$_fv_tmp"
        echo "ERROR: download failed: $_fv_url" >&2
        return 1
    fi
    _fv_got=$(sha256sum "$_fv_tmp" | cut -d' ' -f1)
    if [ "$_fv_got" != "$_fv_want" ]; then
        rm -f "$_fv_tmp"
        echo "ERROR: SHA-256 mismatch for the file downloaded from $_fv_url" >&2
        echo "  expected: $_fv_want" >&2
        echo "  found:    $_fv_got" >&2
        echo "  The download was discarded; $_fv_dest was not created." >&2
        echo "  Either the server now serves a different file or the download was corrupted." >&2
        return 1
    fi
    mv -f "$_fv_tmp" "$_fv_dest"
    echo "verified: $_fv_dest (downloaded, SHA-256 matches)"
    return 0
}

zig_check() {
    _zc_py=${1:-$HOME/hiby/venv/bin/python}
    _zc_got=$("$_zc_py" -m ziglang version 2>/dev/null) || _zc_got=
    if [ "$_zc_got" = "$HIBYOS_ZIG_VERSION" ]; then return 0; fi
    if [ -z "$_zc_got" ]; then
        echo "ERROR: could not run \"$_zc_py -m ziglang version\"." >&2
        echo "  Install the pinned compiler: $_zc_py -m pip install ziglang==$HIBYOS_ZIG_VERSION" >&2
        echo "  (see the \"Pinned toolchain\" notes in tools/wsl/fetch.sh)" >&2
        return 1
    fi
    if [ -n "$HIBYOS_ALLOW_OTHER_ZIG" ]; then
        echo "WARNING: Zig $_zc_got is installed, but the pinned version is $HIBYOS_ZIG_VERSION (HIBYOS_ALLOW_OTHER_ZIG is set, continuing)." >&2
        return 0
    fi
    echo "ERROR: Zig $_zc_got is installed, but the pinned version is $HIBYOS_ZIG_VERSION." >&2
    echo "  Install it with: $_zc_py -m pip install ziglang==$HIBYOS_ZIG_VERSION" >&2
    echo "  or set HIBYOS_ALLOW_OTHER_ZIG=1 to build with the installed version anyway." >&2
    return 1
}
