#!/bin/bash
# hibysetctl (prueba de hibyset.inc.c / orden 'S' de hibymod_disp.so v7): estatico musl hard-float.
# Uso (WSL): bash tools/build_hibysetctl.sh [DIR_SALIDA]   (por defecto out/agent_set)
#            bash tools/build_hibysetctl.sh pc             (version x86_64 en /tmp, pruebas en el PC)
set -e
R=/mnt/c/Users/alber/Downloads/Hiby/hibymod
S=$R/src
if [ "$1" = pc ]; then
  gcc -O1 -g -Wall -Wextra -Wno-unused-function -I$S -o /tmp/hibysetctl_pc $S/hibysetctl.c
  ls -la /tmp/hibysetctl_pc; exit 0
fi
O=${1:-$R/out/agent_set}
mkdir -p "$O"
. "$(dirname "$0")/wsl/fetch.sh"; zig_check   # pinned compiler: ziglang 0.17.1, see tools/wsl/fetch.sh
~/hiby/venv/bin/python -m ziglang cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -O2 -static -s \
  -I$S -Wall -Wextra -Wno-unused-function -o "$O/hibysetctl" $S/hibysetctl.c
ls -la "$O/hibysetctl"
