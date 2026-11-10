#!/bin/sh
# hibymod "ruta rapida": copia a /usr/data (UBIFS, nodos de 4 KB) lo que el reproductor
# lee constantemente del sistema (squashfs, bloques de 128 KB).
# Medido en el aparato: leer una pagina en frio cuesta ~4,7 ms en squashfs y ~0,6 ms en
# UBIFS. Cada fallo de pagina del codigo del reproductor es un tiron visible.
#
#   hibymod_fast.sh setup    copia y verifica (se ejecuta en segundo plano tras arrancar)
#   hibymod_fast.sh ok       sale con 0 si la copia es valida para esta version
#   hibymod_fast.sh remove   borra la copia

FAST=/usr/data/hibymod/fast
BUILD=$(cat /etc/hibymod_build 2>/dev/null)

# librerias que usa hiby_player (de /proc/PID/maps); ld.so queda fuera (ruta fija en el ELF)
LIBS="/lib/libc-2.22.so /lib/libm-2.22.so /lib/libpthread-2.22.so /lib/libgcc_s.so.1
/lib/librt-2.22.so /lib/libdl-2.22.so /lib/libatomic.so.1.1.0 /usr/lib/libasound.so.2.0.0
/usr/lib/libz.so.1.2.11 /usr/lib/libcrypto.so.1.1 /usr/lib/libssl.so.1.1"
# enlaces soname -> fichero (solo cuando el nombre difiere; libgcc_s, libcrypto y libssl
# ya se llaman como su soname)
SONAMES="libc.so.6:libc-2.22.so libm.so.6:libm-2.22.so libpthread.so.0:libpthread-2.22.so
librt.so.1:librt-2.22.so libdl.so.2:libdl-2.22.so
libatomic.so.1:libatomic.so.1.1.0 libasound.so.2:libasound.so.2.0.0 libz.so.1:libz.so.1.2.11"
FONT=/usr/resource/fonts/default.otf
RESDIRS="layout/theme2 layout/theme3 litegui/theme2 litegui/theme3"

ok()
{
    [ -n "$BUILD" ] && [ "$(cat $FAST/.build 2>/dev/null)" = "$BUILD" ] && [ -x $FAST/hiby_player ]
}

copy_check()   # origen destino
{
    cp -p "$1" "$2" && cmp -s "$1" "$2"
}

setup()
{
    ok && return 0
    [ -n "$BUILD" ] || return 1
    rm -rf $FAST
    # espacio: se necesitan ~22 MB sin comprimir; UBIFS comprime, pero se exige margen
    avail=$(df /usr/data | awk 'NR==2{print $4}')
    [ "${avail:-0}" -gt 21000 ] || { echo "sin espacio ($avail KB)"; return 1; }   # ~17 MB en flash (UBIFS comprime) + margen
    mkdir -p $FAST/lib $FAST/res || return 1
    fail() { echo "copia fallida: $1"; rm -rf $FAST; sync; return 1; }
    copy_check /usr/bin/hiby_player $FAST/hiby_player.tmp || { fail hiby_player; return 1; }
    for l in $LIBS; do
        copy_check $l $FAST/lib/$(basename $l) || { fail $l; return 1; }
    done
    for s in $SONAMES; do
        [ "${s%%:*}" = "${s#*:}" ] && continue
        ln -sf ${s#*:} $FAST/lib/${s%%:*} || { fail $s; return 1; }
    done
    # comprobacion final: ninguna libreria puede ser un enlace roto o en bucle
    for l in $FAST/lib/*; do [ -r "$l" ] || { fail "enlace roto $l"; return 1; }; done
    copy_check $FONT $FAST/default.otf || { fail fuente; return 1; }
    for d in $RESDIRS; do
        mkdir -p $FAST/res/$d && cp -rp /usr/resource/$d/. $FAST/res/$d/ || { fail $d; return 1; }
        [ "$(find /usr/resource/$d | wc -l)" = "$(find $FAST/res/$d | wc -l)" ] || { fail "$d incompleto"; return 1; }
    done
    mv $FAST/hiby_player.tmp $FAST/hiby_player || { fail mv; return 1; }
    echo "$BUILD" > $FAST/.build
    sync
    echo "ruta rapida lista ($(du -s $FAST | cut -f1) KB)"
}

case "$1" in
    setup)  setup ;;
    ok)     ok ;;
    remove) rm -rf $FAST; sync ;;
    *)      echo "uso: $0 setup|ok|remove"; exit 1 ;;
esac
