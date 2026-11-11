#!/bin/bash
# Builds the ByOS firmware from the OFFICIAL HiBy firmware that you download yourself.
# ByOS does not redistribute HiBy's firmware: this script modifies your copy, on your computer.
#
# Usage (Linux, WSL on Windows, or a GitHub Codespace):
#   bash tools/build_hibyos.sh /path/to/r3ii_2025.upt        # the official HiBy .upt
# Result:
#   out/r3ii_2025.upt      the ByOS firmware (copy it to the root of the SD card)
#   (the ByOS/ folder for the SD card comes from the GitHub release)
set -e
cd "$(dirname "$0")/.."
UPT="$1"
SUPPORTED_MD5=0d16d09c1458dd2f2cdaf38d0047917a    # official R3 II (2025) firmware, 2 March 2026

if [ ! -f "$UPT" ]; then
    echo "Usage: bash tools/build_hibyos.sh /path/to/r3ii_2025.upt"
    echo "       (the official firmware file downloaded from HiBy's website)"
    exit 1
fi

need() { command -v "$1" >/dev/null 2>&1; }
missing=""
need unsquashfs && need mksquashfs || missing="$missing squashfs-tools"
need unshare || missing="$missing util-linux"
need python3 || missing="$missing python3 python3-venv"
if [ -n "$missing" ]; then
    echo "Missing tools:$missing"
    echo "On Debian/Ubuntu/WSL/Codespaces run:  sudo apt update && sudo apt install -y$missing"
    exit 1
fi

PY=python3
if ! $PY -c "import pycdlib, PIL" 2>/dev/null; then
    echo "== Installing the Python packages (pycdlib, pillow) into .venv"
    [ -x .venv/bin/python ] || $PY -m venv .venv
    .venv/bin/pip -q install pycdlib pillow
    PY=.venv/bin/python
fi

echo "== Checking the official firmware"
got=$(md5sum "$UPT" | cut -d' ' -f1)
if [ "$got" != "$SUPPORTED_MD5" ]; then
    echo "This is not the supported official firmware."
    echo "  your file: $got"
    echo "  expected:  $SUPPORTED_MD5  (HiBy R3 II 2025, firmware of 2 March 2026)"
    echo "ByOS only patches that exact version. Nothing was changed."
    exit 1
fi
echo "OK: supported official firmware."

mkdir -p out
# build.py also checks the original text and md5 of every file it patches, and that the
# result fits the partition.
echo "== Building ByOS (about 1 minute)"
# The image must keep root ownership: use a user namespace, or sudo where namespaces are
# not allowed (some containers).
if unshare -r true 2>/dev/null; then
    unshare -r $PY -I tools/build.py --stock "$UPT" --overlay overlay --out out/r3ii_2025.upt
elif sudo -n true 2>/dev/null; then
    sudo $PY -I tools/build.py --stock "$UPT" --overlay overlay --out out/r3ii_2025.upt
    sudo chown -R "$(id -u):$(id -g)" out
else
    echo "Cannot build: needs either user namespaces ('unshare -r') or sudo."; exit 1
fi
echo
echo "Done: out/r3ii_2025.upt"
echo "Next: copy out/r3ii_2025.upt and the ByOS/ folder from the release to the root of the"
echo "SD card, then on the player: Settings > Firmware update > Via SD card."
