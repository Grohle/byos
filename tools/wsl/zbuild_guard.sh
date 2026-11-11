#!/bin/bash
# hibyguard: static (musl), small (-Os); it is locked in RAM on the player
set -e
. "$(dirname "$0")/fetch.sh"; zig_check   # pinned compiler: ziglang 0.17.1, see tools/wsl/fetch.sh
S=/mnt/c/Users/alber/Downloads/Hiby/hibymod/src
~/hiby/venv/bin/python -m ziglang cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os -static -s -Wall -Wextra -I$S \
  -o /mnt/c/Users/alber/Downloads/Hiby/hibymod/bin/hibyguard $S/hibyguard.c
ls -la /mnt/c/Users/alber/Downloads/Hiby/hibymod/bin/hibyguard
