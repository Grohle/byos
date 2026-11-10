#!/bin/bash
# hibynet: cliente HTTPS estatico (musl, coma flotante por hardware) para letras y caratulas.
#   TLS: mbedTLS 2.28 (TLS 1.2) con MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK, para cargar
#        bajo demanda solo la raiz que hace falta de cacert.pem (RSS bajo con el bundle completo).
#   JPEG: libjpeg 9f (decodificacion escalada linea a linea), PNG via stb_image, salida stb_image_write.
# Salidas: bin/hibynet (mipsel) + bin/cacert.pem ; ~/hiby/net/hibynet-x86 (para medir en PC).
# Uso: tools/wsl/zbuild_net.sh [x86]   (x86 = compila tambien la version de PC)
set -e
R=/mnt/c/Users/alber/Downloads/Hiby/hibymod
S=$R/src
N=$HOME/hiby/net
DL=$HOME/hiby/vm/dl/deps
ZIG="$HOME/hiby/venv/bin/python -m ziglang"
mkdir -p $N/xcc $N/src
# envoltorios del compilador cruzado (hard-float!)
cat > $N/xcc/mipsel-hf-cc <<EOF
#!/bin/sh
exec $ZIG cc -target mipsel-linux-musleabihf -mcpu=mips32r2 "\$@"
EOF
cat > $N/xcc/mipsel-hf-ar <<EOF
#!/bin/sh
exec $ZIG ar "\$@"
EOF
cat > $N/xcc/mipsel-hf-ranlib <<EOF
#!/bin/sh
exec $ZIG ranlib "\$@"
EOF
chmod +x $N/xcc/*

[ -f $DL/mbedtls-2.28.9.tar.gz ] || curl -sfL -o $DL/mbedtls-2.28.9.tar.gz https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v2.28.9.tar.gz
[ -f $DL/jpegsrc.v9f.tar.gz ] || curl -sfL -o $DL/jpegsrc.v9f.tar.gz https://www.ijg.org/files/jpegsrc.v9f.tar.gz

# deps ARCH CC AR RANLIB HOSTTRIPLE
deps() {
  local A=$1 CC=$2 AR=$3 RL=$4 HOST=$5 P=$N/$1
  mkdir -p $P/lib $P/include
  if [ ! -f $P/lib/libmbedtls.a ]; then
    rm -rf $N/src/mbedtls-$A; mkdir -p $N/src/mbedtls-$A
    tar xf $DL/mbedtls-2.28.9.tar.gz -C $N/src/mbedtls-$A --strip-components=1
    cd $N/src/mbedtls-$A
    python3 scripts/config.py set MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK
    python3 scripts/config.py unset MBEDTLS_SSL_SRV_C
    python3 scripts/config.py unset MBEDTLS_SSL_DTLS_HELLO_VERIFY
    python3 scripts/config.py unset MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
    python3 scripts/config.py unset MBEDTLS_SSL_DTLS_ANTI_REPLAY
    python3 scripts/config.py unset MBEDTLS_SSL_PROTO_DTLS
    python3 scripts/config.py unset MBEDTLS_SSL_DTLS_BADMAC_LIMIT
    python3 scripts/config.py unset MBEDTLS_SSL_DTLS_CONNECTION_ID 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_SSL_DTLS_SRTP 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_SSL_CACHE_C
    python3 scripts/config.py unset MBEDTLS_SSL_TICKET_C
    python3 scripts/config.py unset MBEDTLS_SSL_COOKIE_C
    python3 scripts/config.py unset MBEDTLS_PSA_CRYPTO_C
    python3 scripts/config.py unset MBEDTLS_PSA_CRYPTO_STORAGE_C
    python3 scripts/config.py unset MBEDTLS_PSA_ITS_FILE_C
    python3 scripts/config.py unset MBEDTLS_LMS_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_DEBUG_C
    python3 scripts/config.py unset MBEDTLS_SELF_TEST
    python3 scripts/config.py unset MBEDTLS_VERSION_FEATURES
    python3 scripts/config.py unset MBEDTLS_SSL_RENEGOTIATION
    make -C library -j20 libmbedtls.a libmbedx509.a libmbedcrypto.a CC="$CC" AR="$AR" \
      CFLAGS="-Os -ffunction-sections -fdata-sections -w" >/dev/null
    cp library/*.a $P/lib/; cp -r include/mbedtls $P/include/
  fi
  if [ ! -f $P/lib/libjpeg.a ]; then
    rm -rf $N/src/jpeg-$A; mkdir -p $N/src/jpeg-$A
    tar xf $DL/jpegsrc.v9f.tar.gz -C $N/src/jpeg-$A --strip-components=1
    cd $N/src/jpeg-$A
    CC="$CC" AR="$AR" RANLIB="$RL" CFLAGS="-Os -ffunction-sections -fdata-sections" \
      ./configure --host=$HOST --prefix=$P --disable-shared >/dev/null
    make -j20 libjpeg.la >/dev/null
    cp .libs/libjpeg.a $P/lib/; cp jpeglib.h jconfig.h jmorecfg.h jerror.h $P/include/
  fi
}

deps mips $N/xcc/mipsel-hf-cc $N/xcc/mipsel-hf-ar $N/xcc/mipsel-hf-ranlib mipsel-linux-musl
[ "$1" = "x86" ] && deps x86 gcc ar ranlib x86_64-linux-gnu

# bundle de CA de Mozilla (via curl.se), se distribuye junto al binario
[ -s $R/bin/cacert.pem ] || curl -sfL -o $R/bin/cacert.pem https://curl.se/ca/cacert.pem

CF="-Os -Wno-format-truncation -ffunction-sections -fdata-sections -Wall -Wno-unused-function -I$HOME/hiby/third"
$N/xcc/mipsel-hf-cc $CF -static -s -Wl,--gc-sections -I$N/mips/include -o $R/bin/hibynet $S/hibynet.c \
  -L$N/mips/lib -lmbedtls -lmbedx509 -lmbedcrypto -ljpeg -lm
ls -la $R/bin/hibynet $R/bin/cacert.pem
file $R/bin/hibynet
if [ "$1" = "x86" ]; then
  gcc $CF -s -Wl,--gc-sections -I$N/x86/include -o $N/hibynet-x86 $S/hibynet.c \
    -L$N/x86/lib -lmbedtls -lmbedx509 -lmbedcrypto -ljpeg -lm
  ls -la $N/hibynet-x86
fi
