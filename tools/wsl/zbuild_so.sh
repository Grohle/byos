#!/bin/bash
# Compila una libreria compartida compatible con hiby_player (glibc 2.22, MIPS32r2 hard-float).
set -e
src=$1; out=$2
~/hiby/venv/bin/python -m ziglang cc -target mipsel-linux-gnueabihf.2.22 -mcpu=mips32r2 \
  -shared -fPIC -O2 -s -o "$out" "$src" -ldl -lpthread
file "$out"; ls -la "$out"
readelf -d "$out" | grep -E 'NEEDED'
readelf -V "$out" | grep -oE 'GLIBC_[0-9.]+' | sort -u | tr '\n' ' '; echo
