#!/bin/bash
# Dependencias estaticas mipsel/musl para NetSurf: zlib, mbedtls, curl, libpng, libjpeg, expat
set -e
X=~/hiby/vm/xcc; chmod +x $X/*; export PATH=$X:$PATH
P=~/hiby/vm/xsysroot; mkdir -p $P ~/hiby/vm/dl/deps; D=~/hiby/vm/dl/deps; B=~/hiby/vm/src/deps; mkdir -p $B
export CC=mipsel-hiby-cc AR=mipsel-hiby-ar RANLIB=mipsel-hiby-ranlib CFLAGS="-Os -ffunction-sections -fdata-sections"
cd $D
get(){ [ -f "$(basename $1)" ] || curl -sfLO "$1"; }
get https://zlib.net/fossils/zlib-1.3.1.tar.gz
[ -f mbedtls-2.28.9.tar.gz ] || curl -sfL -o mbedtls-2.28.9.tar.gz https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v2.28.9.tar.gz
get https://curl.se/download/curl-8.11.1.tar.xz
get https://download.sourceforge.net/libpng/libpng-1.6.44.tar.xz
get https://www.ijg.org/files/jpegsrc.v9f.tar.gz
get https://github.com/libexpat/libexpat/releases/download/R_2_6_4/expat-2.6.4.tar.xz
ls -la
cd $B
[ -f $P/lib/libz.a ] || { tar xf $D/zlib-1.3.1.tar.gz; cd zlib-1.3.1; ./configure --static --prefix=$P >/dev/null; make -j20 >/dev/null; make install >/dev/null; cd ..; }
[ -f $P/lib/libmbedtls.a ] || { tar xf $D/mbedtls-2.28.9.tar.gz; cd mbedtls-2.28.9; make -j20 lib CC=$CC AR=$AR >/dev/null; make install DESTDIR=$P >/dev/null; cd ..; }
[ -f $P/lib/libcurl.a ] || { tar xf $D/curl-8.11.1.tar.xz; cd curl-8.11.1; ./configure --host=mipsel-linux-musl --prefix=$P --disable-shared --enable-static --with-mbedtls=$P --with-zlib=$P \
  --disable-ftp --disable-file --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap --disable-smb --disable-smtp --disable-gopher --disable-mqtt \
  --disable-manual --disable-ntlm --disable-unix-sockets --without-libpsl --without-brotli --without-zstd --without-nghttp2 --without-libidn2 --disable-docs --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt >/dev/null; make -j20 >/dev/null; make install >/dev/null; cd ..; }
[ -f $P/lib/libpng.a ] || { tar xf $D/libpng-1.6.44.tar.xz; cd libpng-1.6.44; CPPFLAGS=-I$P/include LDFLAGS=-L$P/lib ./configure --host=mipsel-linux-musl --prefix=$P --disable-shared >/dev/null; make -j20 >/dev/null; make install >/dev/null; cd ..; }
[ -f $P/lib/libjpeg.a ] || { tar xf $D/jpegsrc.v9f.tar.gz; cd jpeg-9f; ./configure --host=mipsel-linux-musl --prefix=$P --disable-shared >/dev/null; make -j20 >/dev/null; make install >/dev/null; cd ..; }
[ -f $P/lib/libexpat.a ] || { tar xf $D/expat-2.6.4.tar.xz; cd expat-2.6.4; ./configure --host=mipsel-linux-musl --prefix=$P --disable-shared --without-docbook --without-examples --without-tests >/dev/null; make -j20 >/dev/null; make install >/dev/null; cd ..; }
ls -la $P/lib/*.a
