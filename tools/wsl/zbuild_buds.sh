#!/bin/bash
# hibybudsd (daemon de auriculares) + hibyshell con la app Auriculares, estaticos (musl, coma
# flotante por hardware), en out/agent_buds/ (nunca en bin/). Ver docs/HEADPHONES.md.
#   tools/wsl/zbuild_buds.sh          ambos
#   tools/wsl/zbuild_buds.sh daemon   solo el daemon
set -e
. "$(dirname "$0")/fetch.sh"; zig_check   # pinned compiler: ziglang 0.17.1, see tools/wsl/fetch.sh
R=/mnt/c/Users/alber/Downloads/Hiby/hibymod
S=$R/src
O=$R/out/agent_buds
ZIG="$HOME/hiby/venv/bin/python -m ziglang cc -target mipsel-linux-musleabihf -mcpu=mips32r2"
mkdir -p $O
$ZIG -Os -static -s -Wall -Wextra -Wno-unused-function -o $O/hibybudsd $S/hibybudsd.c
ls -la $O/hibybudsd
[ "$1" = daemon ] && exit 0
# misma orden que tools/wsl/zbuild_shell.sh, con -o propio
$ZIG -O2 -static -s \
  -I$S -I$HOME/hiby/third -Wall -Wno-unused-function -Wno-misleading-indentation -Wno-unused-result \
  -o $O/hibyshell $S/hibyshell.c -lm
ls -la $O/hibyshell
