#!/bin/bash
# hibyplay (prueba de nowplaying/playctl): estatico musl hard-float para el R3 II.
# Uso (WSL): bash tools/build_hibyplay.sh   [pc = version x86_64 para pruebas en el PC]
set -e
S=/mnt/c/Users/alber/Downloads/Hiby/hibymod/src
B=/mnt/c/Users/alber/Downloads/Hiby/hibymod/bin
if [ "$1" = pc ]; then
  gcc -O1 -g -Wall -Wno-unused-function -I$S -o /tmp/hibyplay_pc $S/hibyplay.c
  ls -la /tmp/hibyplay_pc; exit 0
fi
. "$(dirname "$0")/wsl/fetch.sh"; zig_check   # pinned compiler: ziglang 0.17.1, see tools/wsl/fetch.sh
~/hiby/venv/bin/python -m ziglang cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -O2 -static -s \
  -I$S -Wall -Wno-unused-function -Wno-unused-result -o $B/hibyplay $S/hibyplay.c
ls -la $B/hibyplay
