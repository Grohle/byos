#!/bin/bash
# hibynet: cliente HTTPS estatico (musl, coma flotante por hardware) para letras y caratulas.
#   TLS: mbedTLS 2.28 (TLS 1.2) con MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK, para cargar
#        bajo demanda solo la raiz que hace falta de cacert.pem (RSS bajo con el bundle completo).
#   JPEG: libjpeg 9f (decodificacion escalada linea a linea), PNG via stb_image, salida stb_image_write.
# Salidas: bin/hibynet (mipsel) + bin/cacert.pem ; ~/hiby/net/hibynet-x86 (para medir en PC).
# Uso: tools/wsl/zbuild_net.sh [x86]   (x86 = compila tambien la version de PC)
set -e
. "$(dirname "$0")/fetch.sh"
zig_check   # pinned compiler: ziglang 0.17.1, see tools/wsl/fetch.sh
R=/mnt/c/Users/alber/Downloads/Hiby/hibymod
S=$R/src
N=$HOME/hiby/net
DL=$HOME/hiby/vm/dl/deps
ZIG="$HOME/hiby/venv/bin/python -m ziglang"
mkdir -p $N/xcc $N/src $DL
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

# pinned sources (exact version + SHA-256, checked on every run): mbedTLS 2.28.9 and libjpeg 9f
fetch_verify https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v2.28.9.tar.gz e4dbcf86a4fb31506482888560f02b161e0ecfb82fee0643abcfc86abee5817e $DL/mbedtls-2.28.9.tar.gz
fetch_verify https://www.ijg.org/files/jpegsrc.v9f.tar.gz 04705c110cb2469caa79fb71fba3d7bf834914706e9641a4589485c1f832565b $DL/jpegsrc.v9f.tar.gz

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
    python3 scripts/config.py unset MBEDTLS_SSL_PROTO_TLS1 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_SSL_PROTO_TLS1_1 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_SSL_CBC_RECORD_SPLITTING 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_DES_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_CAMELLIA_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_ARIA_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_BLOWFISH_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_XTEA_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_ARC4_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_RIPEMD160_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_CCM_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_CMAC_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_NIST_KW_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_HKDF_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_ECJPAKE_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_KEY_EXCHANGE_PSK_ENABLED 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_DHM_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_PKCS12_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_PKCS5_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_X509_CRL_PARSE_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_X509_CSR_PARSE_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_X509_CREATE_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_X509_CRT_WRITE_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_X509_CSR_WRITE_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_PK_WRITE_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_CERTS_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_PEM_WRITE_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_TIMING_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_HAVEGE_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_PADLOCK_C 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_SSL_DTLS_SRTP 2>/dev/null || true
    python3 scripts/config.py unset MBEDTLS_SSL_TRUNCATED_HMAC 2>/dev/null || true
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
# pinned to the dated bundle of 2026-11-05 (https://curl.se/docs/caextract.html) and its SHA-256;
# to move to a newer bundle, change the date and the hash here and delete bin/cacert.pem
fetch_verify https://curl.se/ca/cacert-2026-11-05.pem 427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285 $R/bin/cacert.pem

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
