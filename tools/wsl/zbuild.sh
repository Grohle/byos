#!/bin/bash
# Compila un .c para el HiBy (XBurst MIPS32r2 LE, soft-float, musl estatico).
set -e
. "$(dirname "$0")/fetch.sh"; zig_check   # pinned compiler: ziglang 0.17.1, see tools/wsl/fetch.sh
src=$1; out=$2
~/hiby/venv/bin/python -m ziglang cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os -static -s -o "$out" "$src"
file "$out"; ls -la "$out"
