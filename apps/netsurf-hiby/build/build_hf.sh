#!/bin/bash
# build_hf.sh - NetSurf 3.11 estatico mipsel musl COMA FLOTANTE HARD (para el .hpk "Navegador").
# Usa las fuentes de netsurf-all-3.11 del arnes (~/hiby/vm/src) copiadas a ~/hiby/nshf (no toca el
# arbol soft-float) y reconstruye zlib, mbedtls, curl, libpng, libjpeg y expat en hard-float.
# Uso (WSL): bash apps/netsurf-hiby/build/build_hf.sh [deps|ns|all]   (por defecto all)
set -e
H=/mnt/c/Users/alber/Downloads/Hiby/hibymod
R=~/hiby/nshf; X=$R/xcc; P=$R/xsysroot; B=$R/deps; D=~/hiby/vm/dl/deps
mkdir -p $X $P $B $R/logs
for t in gcc cc; do printf '#!/bin/sh\nexec /home/alber/hiby/venv/bin/python -m ziglang cc -target mipsel-linux-musleabihf -mcpu=mips32r2 "$@"\n' > $X/mipsel-hiby-$t; done
printf '#!/bin/sh\nexec /home/alber/hiby/venv/bin/python -m ziglang cc -E -target mipsel-linux-musleabihf "$@"\n' > $X/mipsel-hiby-cpp
printf '#!/bin/sh\nexec /home/alber/hiby/venv/bin/python -m ziglang ar "$@"\n' > $X/mipsel-hiby-ar
printf '#!/bin/sh\nexec /home/alber/hiby/venv/bin/python -m ziglang ranlib "$@"\n' > $X/mipsel-hiby-ranlib
chmod +x $X/*; export PATH=$X:$PATH
W=${1:-all}
if [ $W = deps -o $W = all ]; then
 (
 export CC=mipsel-hiby-cc AR=mipsel-hiby-ar RANLIB=mipsel-hiby-ranlib CFLAGS="-Os -ffunction-sections -fdata-sections"
 cd $B
 [ -f $P/lib/libz.a ] || { tar xf $D/zlib-1.3.1.tar.gz; cd zlib-1.3.1; ./configure --static --prefix=$P >/dev/null; make -j20 >/dev/null; make install >/dev/null; cd ..; }
 [ -f $P/lib/libmbedtls.a ] || { tar xf $D/mbedtls-2.28.9.tar.gz; cd mbedtls-2.28.9; make -j20 lib CC=$CC AR=$AR >/dev/null; make install DESTDIR=$P >/dev/null; cd ..; }
 [ -f $P/lib/libcurl.a ] || { tar xf $D/curl-8.11.1.tar.xz; cd curl-8.11.1; ./configure --host=mipsel-linux-musl --prefix=$P --disable-shared --enable-static --with-mbedtls=$P --with-zlib=$P \
  --disable-ftp --disable-file --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap --disable-smb --disable-smtp --disable-gopher --disable-mqtt \
  --disable-manual --disable-ntlm --disable-unix-sockets --without-libpsl --without-brotli --without-zstd --without-nghttp2 --without-libidn2 --disable-docs --without-ca-path --with-ca-bundle=/usr/data/hibymod/shell/cacert.pem >/dev/null; make -j20 >/dev/null; make install >/dev/null; cd ..; }
 [ -f $P/lib/libpng.a ] || { tar xf $D/libpng-1.6.44.tar.xz; cd libpng-1.6.44; CPPFLAGS=-I$P/include LDFLAGS=-L$P/lib ./configure --host=mipsel-linux-musl --prefix=$P --disable-shared >/dev/null; make -j20 >/dev/null; make install >/dev/null; cd ..; }
 [ -f $P/lib/libjpeg.a ] || { tar xf $D/jpegsrc.v9f.tar.gz; cd jpeg-9f; ./configure --host=mipsel-linux-musl --prefix=$P --disable-shared >/dev/null; make -j20 >/dev/null; make install >/dev/null; cd ..; }
 [ -f $P/lib/libexpat.a ] || { tar xf $D/expat-2.6.4.tar.xz; cd expat-2.6.4; ./configure --host=mipsel-linux-musl --prefix=$P --disable-shared --without-docbook --without-examples --without-tests >/dev/null; make -j20 >/dev/null; make install >/dev/null; cd ..; }
 ls -la $P/lib/*.a
 ) > $R/logs/deps.log 2>&1 || { echo "deps FALLO"; tail -30 $R/logs/deps.log; exit 1; }
 echo deps ok
fi
if [ $W = ns -o $W = all ]; then
 N=$R/netsurf-all-3.11
 if [ ! -d $N ]; then cp -a ~/hiby/vm/src/netsurf-all-3.11 $N; cd $N; find . -maxdepth 2 -type d -name 'build-*' -exec rm -rf {} +; rm -rf netsurf/build inst-framebuffer; fi
 T=~/hiby/vm/xtc; export PATH=$PATH:$T/usr/bin BISON_PKGDATADIR=$T/usr/share/bison M4=$T/usr/bin/m4 LD_LIBRARY_PATH=$T/usr/lib/x86_64-linux-gnu
 cp $H/apps/netsurf-hiby/build/nsfb_hiby.c $N/libnsfb/src/surface/hiby.c
 python3 $H/apps/netsurf-hiby/build/patch_gui.py $N/netsurf/frontends/framebuffer/gui.c
 grep -q hiby.c $N/libnsfb/src/surface/Makefile || sed -i 's/^SURFACE_HANDLER_yes := surface.c ram.c/SURFACE_HANDLER_yes := surface.c ram.c hiby.c/' $N/libnsfb/src/surface/Makefile
 export PKG_CONFIG_LIBDIR=$P/lib/pkgconfig:$N/inst-framebuffer/lib/pkgconfig
 cd $N; export CFLAGS="-I$P/include -Os -ffunction-sections -fdata-sections"
 cat > netsurf/Makefile.config <<EOC
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
CFLAGS += -I$P/include -Os -ffunction-sections -fdata-sections
LDFLAGS += -static -L$P/lib -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -Wl,--gc-sections -Wl,-z,stack-size=262144 -s
EOC
 # libnsfb ya instalada: forzar que se reinstale con la surface nueva
 rm -f $N/inst-framebuffer/lib/libnsfb.a $N/libnsfb/build-*/src/surface/hiby.o 2>/dev/null || true
 rm -f $N/netsurf/nsfb $N/inst-framebuffer/build-stamp
 make TARGET=framebuffer HOST=mipsel-hiby BUILD=x86_64-linux-gnu CC=mipsel-hiby-cc HOST_CC=gcc BUILD_CC=gcc AR=mipsel-hiby-ar \
  BUILD_CFLAGS="-I$HOME/hiby/vm/hostsysroot/include" BUILD_LDFLAGS="-L$HOME/hiby/vm/hostsysroot/lib" BUILD_LIBPNG_CFLAGS="-I$HOME/hiby/vm/hostsysroot/include" BUILD_LIBPNG_LDFLAGS="-L$HOME/hiby/vm/hostsysroot/lib -lpng16 -lz -lm" PREFIX=$N/inst-framebuffer -j16 > $R/logs/ns.log 2>&1 || { echo "ns FALLO"; grep -nE "error" $R/logs/ns.log | head -30; tail -20 $R/logs/ns.log; exit 1; }
 ls -la $N/netsurf/nsfb; readelf -A $N/netsurf/nsfb | grep "FP ABI"
fi
