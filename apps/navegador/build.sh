#!/bin/bash
# build.sh (WSL) - empaqueta la app "Navegador": lanzador + nsfb hard-float + recursos -> out/apps/navegador.hpk
# Antes: bash apps/netsurf-hiby/build/build_hf.sh (genera ~/hiby/nshf/.../netsurf/nsfb)
set -e
H=/mnt/c/Users/alber/Downloads/Hiby/hibymod
. $H/tools/wsl/fetch.sh; zig_check   # pinned compiler: ziglang 0.17.1, see tools/wsl/fetch.sh
A=$H/apps/navegador
N=~/hiby/nshf/netsurf-all-3.11/netsurf
ST=~/hiby/build/apps/navegador
rm -rf $ST; mkdir -p $ST/bin $ST/res $H/out/apps
~/hiby/venv/bin/python -m ziglang cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os -static -s -Wall \
  -o $ST/bin/navegador $A/navegador.c
cp $N/nsfb $ST/bin/nsfb
R=$N/frontends/framebuffer/res
cp -L $R/Messages $R/adblock.css $R/default.css $R/internal.css $R/quirks.css $R/favicon.png $R/credits.html $R/licence.html $R/welcome.html $ST/res/
mkdir -p $ST/res/en; cp -L $R/en/Messages $ST/res/en/
cp -rL $R/icons $R/throbber $R/pointers $ST/res/
cp $A/manifest.ini $A/icon.png $A/ca.pem $ST/
~/hiby/venv/bin/python $H/tools/hpk.py build $ST -o $H/out/apps/navegador.hpk
ls -la $ST/bin $H/out/apps/navegador.hpk; du -sh $ST
