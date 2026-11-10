#!/bin/bash
# NetSurf 3.11 framebuffer, estatico mipsel musl, surface "linux" propia (fb0 + touch goodix + teclas HiBy)
X=~/hiby/vm/xcc
for t in gcc cc; do printf '#!/bin/sh\nexec /home/alber/hiby/venv/bin/python -m ziglang cc -target mipsel-linux-musleabi -mcpu=mips32r2 -msoft-float "$@"\n' > $X/mipsel-hiby-$t; done
printf '#!/bin/sh\nexec /home/alber/hiby/venv/bin/python -m ziglang cc -E -target mipsel-linux-musleabi "$@"\n' > $X/mipsel-hiby-cpp
chmod +x $X/*; T=~/hiby/vm/xtc; export PATH=$X:$PATH:$T/usr/bin BISON_PKGDATADIR=$T/usr/share/bison M4=$T/usr/bin/m4 LD_LIBRARY_PATH=$T/usr/lib/x86_64-linux-gnu
P=~/hiby/vm/xsysroot
N=~/hiby/vm/src/netsurf-all-3.11
cp ~/hiby/vm/tools/nsfb_hiby.c $N/libnsfb/src/surface/hiby.c
grep -q hiby.c $N/libnsfb/src/surface/Makefile || sed -i 's/^SURFACE_HANDLER_yes := surface.c ram.c/SURFACE_HANDLER_yes := surface.c ram.c hiby.c/' $N/libnsfb/src/surface/Makefile
export PKG_CONFIG_LIBDIR=$P/lib/pkgconfig:$N/inst-framebuffer/lib/pkgconfig
cd $N; export CFLAGS="-I$P/include -Os -ffunction-sections -fdata-sections"
cat > netsurf/Makefile.config <<'EOC'
override NETSURF_FB_FRONTEND := linux
override NETSURF_FB_FONTLIB := internal
override NETSURF_USE_DUKTAPE := NO
override NETSURF_USE_NSSVG := NO
override NETSURF_USE_ROSPRITE := NO
override NETSURF_USE_WEBP := NO
override NETSURF_USE_OPENSSL := NO
override NETSURF_USE_HARU_PDF := NO
override NETSURF_USE_VIDEO := NO
override NETSURF_USE_UTF8PROC := YES
override NETSURF_USE_JPEG := YES
override NETSURF_USE_PNG := YES
override NETSURF_USE_BMP := YES
override NETSURF_USE_GIF := YES
CFLAGS += -I$(HOME)/hiby/vm/xsysroot/include -Os -ffunction-sections -fdata-sections
LDFLAGS += -static -L$(HOME)/hiby/vm/xsysroot/lib -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -Wl,--gc-sections -s
EOC
make TARGET=framebuffer HOST=mipsel-hiby BUILD=x86_64-linux-gnu CC=mipsel-hiby-cc HOST_CC=gcc BUILD_CC=gcc AR=mipsel-hiby-ar \
  BUILD_CFLAGS="-I$HOME/hiby/vm/hostsysroot/include" BUILD_LDFLAGS="-L$HOME/hiby/vm/hostsysroot/lib" BUILD_LIBPNG_CFLAGS="-I$HOME/hiby/vm/hostsysroot/include" BUILD_LIBPNG_LDFLAGS="-L$HOME/hiby/vm/hostsysroot/lib -lpng16 -lz -lm" PREFIX=$N/inst-framebuffer -j16 "$@" > ~/hiby/vm/logs/nsbuild.log 2>&1
echo "make rc=$?"
grep -nE "error|Error" ~/hiby/vm/logs/nsbuild.log | head -30
tail -5 ~/hiby/vm/logs/nsbuild.log
ls -la $N/netsurf/nsfb $N/netsurf/netsurf* 2>/dev/null
