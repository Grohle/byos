#!/bin/sh
# hibymod: supervisor de hiby_player para HiBy R3 II 2025.
# El original hacia "hiby_player; sleep 1; reboot": cualquier cierre o crash
# del reproductor reiniciaba el aparato sin dejar rastro.
#  - Informe de cada fallo en la SD (hibymod/logs) antes del reinicio.
#  - Tema hibymod aplicado con montajes bind (el sistema no se modifica).
#  - Modo seguro: 3 arranques seguidos sin que el reproductor aguante 2 min
#    -> se arranca con el tema de fabrica hasta "hibymod safe off".

MODDIR=/tmp/hibymod
SD=/data/mnt/sd_0
RES=/usr/resource
mkdir -p $MODDIR

killall    hiby_player    &>/dev/null
killall -9 hiby_player    &>/dev/null

if [ -f "/usr/bin/batd" ]; then
killall    batd    &>/dev/null
killall -9 batd    &>/dev/null
/usr/bin/batd -v -s -t5 -o /mnt/sd_0/batlog.txt &
fi

[ -x /usr/bin/hibymod_tune.sh ] && /usr/bin/hibymod_tune.sh

# monitor de memoria solo en modo diagnostico (hibymod debug on): cero procesos residentes por defecto
if [ -f /data/.hibymod_debug ] && [ -x /usr/bin/hibymod_memmon.sh ] && [ ! -f $MODDIR/memmon.pid ]; then
    /usr/bin/hibymod_memmon.sh &
    echo $! > $MODDIR/memmon.pid
fi

# ---------------------------------------------------------------- modo seguro
fails=$(cat /data/.hibymod_bootfail 2>/dev/null)
case "$fails" in ''|*[!0-9]*) fails=0 ;; esac
fails=$((fails + 1))
echo $fails > /data/.hibymod_bootfail
if [ $fails -ge 3 ] && [ ! -f /data/.hibymod_safe ]; then
    echo "auto: $fails arranques fallidos" > /data/.hibymod_safe
fi
sync
# si el reproductor sigue vivo a los 2 minutos, el arranque cuenta como bueno
( sleep 120; pidof hiby_player >/dev/null && echo 0 > /data/.hibymod_bootfail && sync ) &

# ---------------------------------------------------------------- instalador HiByOS
# Instala/actualiza shell, libreria y tema desde la SD (HiByOS/payload) ANTES del tema y del
# reproductor. Sin nada que hacer no espera a la SD (solo si HiByOS aun no esta instalado).
# Nunca en modo seguro; /data/.hibymod_noinstall lo desactiva. Ver hibyos_install.sh.
if [ ! -f /data/.hibymod_safe ] && [ ! -f /data/.hibymod_noinstall ] && [ -x /usr/bin/hibyos_install.sh ]; then
    /usr/bin/hibyos_install.sh boot </dev/null >/dev/null 2>&1
fi

# ---------------------------------------------------------------- ruta rapida
# Reproductor, librerias, fuente e imagenes copiados a /usr/data (ver hibymod_fast.sh).
FASTOK=0
FAST=/usr/data/hibymod/fast
if [ ! -f /data/.hibymod_safe ] && [ ! -f /data/.hibymod_nofast ] && /usr/bin/hibymod_fast.sh ok; then
    FASTOK=1
fi

# ---------------------------------------------------------------- tema
apply_theme()
{
    if [ -f /data/.hibymod_safe ]; then
        echo "tema: fabrica (modo seguro: $(cat /data/.hibymod_safe))" > $MODDIR/theme.txt
        return
    fi
    # el tema vive en la particion de datos (el rootfs solo tiene ~2,5 MB libres).
    # Sin tema propio se usa la copia rapida de las imagenes de fabrica (UBIFS).
    src=/usr/data/hibymod/theme
    [ -d $src ] || { [ "$FASTOK" = 1 ] && src=/usr/data/hibymod/fast/res; }
    if [ -f /data/.hibymod_theme_dev ]; then
        # esperar hasta 10 s a que sys_server monte la SD
        i=0
        while [ $i -lt 20 ] && ! grep -qE " (/usr)?/data/mnt/sd_0 " /proc/mounts; do sleep 0.5; i=$((i + 1)); done
        [ -d $SD/hibymod/theme ] && src=$SD/hibymod/theme
    fi
    n=0
    for sub in layout/theme2 layout/theme3 litegui/theme2 litegui/theme3 fonts str; do
        if [ -d "$src/$sub" ] && [ -d "$RES/$sub" ] && ! grep -q " $RES/$sub " /proc/mounts; then
            mount -o bind "$src/$sub" "$RES/$sub" && n=$((n + 1))
        fi
    done
    # ficheros de configuracion sueltos (set_functions.json, config.json...) para
    # probar opciones sin flashear: /usr/data/hibymod/override/<fichero>
    for f in /usr/data/hibymod/override/*; do
        [ -f "$f" ] || continue
        dst=$RES/$(basename "$f")
        if [ -f "$dst" ] && ! grep -q " $dst " /proc/mounts; then
            mount -o bind "$f" "$dst" && n=$((n + 1))
        fi
    done
    echo "tema: $src ($n montajes)" > $MODDIR/theme.txt
}
apply_theme

[ -w /proc/self/oom_score_adj ] && echo -300 > /proc/self/oom_score_adj

# ---------------------------------------------------------------- reproductor
# Probado en el aparato: relanzar hiby_player sin reiniciar deja la pantalla
# congelada (el hardware de video no se reinicia). Por eso, como el firmware
# oficial, cualquier cierre termina en reinicio; hibymod solo anade el informe.
t0=$(cut -d. -f1 /proc/uptime)
# comandos rapidos para lo que el reproductor lanza en bucle (wpa_cli cada 3 s con Wi-Fi):
# solo afectan a los procesos que crea el reproductor; fuera del modo seguro
[ -f /data/.hibymod_safe ] || export PATH=/usr/lib/hibymod/bin:$PATH
# Control de pantalla del launcher de hibymod (libreria en /usr/data, sin flashear).
# Leccion aprendida (v1 dejo la pantalla en negro): solo se carga
#   - para UN arranque si existe /usr/data/hibymod/preload_once (se borra ANTES de
#     lanzar el reproductor: si algo falla, reiniciar basta para volver a la normalidad), o
#   - siempre si existe /usr/data/hibymod/preload_ok (solo tras validarla en el aparato).
# Nunca en modo seguro; /data/.hibymod_nopreload (orden SIN_LAUNCHER en la SD) lo impide siempre.
PRELOAD=/usr/data/hibymod/preload/hibymod_disp.so
use_preload=0
if [ -f /usr/data/hibymod/preload_once ]; then
    rm -f /usr/data/hibymod/preload_once; sync
    use_preload=1
elif [ -f /usr/data/hibymod/preload_ok ]; then
    use_preload=1
fi
if [ $use_preload = 1 ] && [ ! -f /data/.hibymod_safe ] && [ ! -f /data/.hibymod_nopreload ] && [ -f $PRELOAD ]; then
    export LD_PRELOAD=$PRELOAD
    echo "control de pantalla: $PRELOAD" >> $MODDIR/theme.txt
    # hibyshell: gestos del sistema, hoja de Now Playing, boton de inicio, aviso de volumen.
    # Se relanza si cae (como mucho 5 veces); si no esta, el reproductor funciona igual
    # (la libreria devuelve tactil y pantalla al reproductor). Desactivar: /usr/data/hibymod/no_shell
    SHELLBIN=/usr/data/hibymod/shell/hibyshell
    if [ -x $SHELLBIN ] && [ ! -f /usr/data/hibymod/no_shell ]; then
        (
            n=0
            while [ $n -lt 5 ]; do
                sleep 3
                s=$(cut -d. -f1 /proc/uptime)
                LD_PRELOAD= $SHELLBIN </dev/null >/dev/null 2>&1
                rc=$?
                e=$(cut -d. -f1 /proc/uptime)
                echo "hibyshell salio (rc=$rc) tras $((e - s)) s" >> $MODDIR/shell_exits.txt
                # solo cuentan los fallos seguidos (un shell que funciono 2 min no es un bucle)
                if [ $((e - s)) -gt 120 ]; then n=0; else n=$((n + 1)); fi
            done
        ) &
        echo "shell: $SHELLBIN" >> $MODDIR/theme.txt
    fi
fi
# El inicio de fabrica es un "espejo" del launcher (iconos transparentes + imagen del
# launcher). Si el launcher no va a estar (sin libreria, sin shell o desactivado), se
# devuelven sus iconos y su vista originales para que siga siendo usable.
MB=/usr/data/hibymod/mirror_bak
if [ -d $MB ] && [ ! -f /data/.hibymod_safe ]; then
    if [ -z "$LD_PRELOAD" ] || [ -f /usr/data/hibymod/no_shell ] || [ -f /usr/data/hibymod/no_launcher ] || [ ! -x /usr/data/hibymod/shell/hibyshell ]; then
        for f in $MB/*.png; do
            [ -f "$f" ] || continue
            d=$RES/litegui/theme3/launcher/$(basename "$f")
            [ -f "$d" ] && mount -o bind "$f" "$d"
        done
        [ -f /usr/data/hibymod/launcher_view.bak ] && mount -o bind /usr/data/hibymod/launcher_view.bak $RES/layout/theme3/launcher/hiby_launcher_apps.view
        echo "inicio: iconos de fabrica (launcher desactivado)" >> $MODDIR/theme.txt
    fi
fi
if [ "$FASTOK" = 1 ]; then
    grep -q " /usr/resource/fonts/default.otf " /proc/mounts || mount -o bind $FAST/default.otf /usr/resource/fonts/default.otf
    echo "reproductor: ruta rapida (/usr/data)" >> $MODDIR/theme.txt
    LD_LIBRARY_PATH=$FAST/lib $FAST/hiby_player
else
    echo "reproductor: sistema" >> $MODDIR/theme.txt
    /usr/bin/hiby_player
fi
rc=$?
t1=$(cut -d. -f1 /proc/uptime)
if [ "$rc" = "0" ]; then
    [ -x /usr/bin/hibymod_crashlog.sh ] && /usr/bin/hibymod_crashlog.sh exit0 "$rc" "$t0" "$t1"
else
    [ -x /usr/bin/hibymod_crashlog.sh ] && /usr/bin/hibymod_crashlog.sh crash "$rc" "$t0" "$t1"
    # reinicio por fallo: no debe contar como apagado limpio
    touch $MODDIR/crash_reboot
fi

rm -f /data/.hibymod_running
sync
sleep 1
reboot
