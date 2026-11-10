#!/bin/bash
# zbuild_app.sh NOMBRE - compila apps/NOMBRE/*.c (estatico, musl, coma flotante hard, mips32r2)
# con el SDK de src/sdk y empaqueta apps/NOMBRE/{manifest.ini,icon.png,data/} en out/apps/NOMBRE.hpk
set -e
. "$(dirname "$0")/fetch.sh"; zig_check   # pinned compiler: ziglang 0.17.1, see tools/wsl/fetch.sh
N=${1:?uso: zbuild_app.sh NOMBRE}
H=/mnt/c/Users/alber/Downloads/Hiby/hibymod
A=$H/apps/$N
ST=$HOME/hiby/build/apps/$N
rm -rf "$ST"; mkdir -p "$ST/bin" "$H/out/apps"
~/hiby/venv/bin/python -m ziglang cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os -static -s \
  -I$H/src/sdk -I$A -I$HOME/hiby/third -Wall -Wno-unused-function \
  -o "$ST/bin/$N" $A/*.c -lm
cp "$A/manifest.ini" "$ST/"
[ -f "$A/icon.png" ] && cp "$A/icon.png" "$ST/"
[ -d "$A/data" ] && cp -r "$A/data" "$ST/"
~/hiby/venv/bin/python $H/tools/hpk.py build "$ST" -o "$H/out/apps/$N.hpk"
ls -la "$ST/bin/$N" "$H/out/apps/$N.hpk"
