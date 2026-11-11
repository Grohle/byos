#!/bin/sh
# HIBYOS_SYS_REV=2
# ByOS: instala/actualiza en /usr/data/hibymod el payload de la SD (ByOS/payload).
# El rootfs no tiene sitio: shell, binarios, libreria de pantalla y tema viven en la
# particion de datos (UBIFS, sobrevive al flasheo). Ver docs/INSTALLER.md.
#
#   hibyos_install.sh boot      antes del reproductor (hiby_player.sh). Sin nada que hacer
#                               no espera a la SD; solo la espera (como mucho $WAIT s) si
#                               ByOS aun no esta instalado.
#   hibyos_install.sh late      desde S95hibymod con la SD ya montada. Sale con 0 si ha
#                               instalado (el que llama reinicia), 1 si no habia nada que hacer.
#   hibyos_install.sh install   a mano (ADB): instala aunque la version sea la misma.
#   hibyos_install.sh status    versiones instalada y de la SD
#   hibyos_install.sh sysok NOMBRE  0 si /usr/data/hibymod/sys/NOMBRE es un sustituto valido
#                               de ese script del rootfs (ver "sustitutos" abajo, docs/OTA.md)
#
# Garantias: verifica el MANIFEST.md5 antes de tocar nada; cada fichero se copia a .new,
# se compara y se renombra (los binarios en uso no se rompen); payload_version se escribe
# lo ultimo (una instalacion cortada se repite); el estado del usuario (launcher.conf,
# wall_custom.raw, power.conf, audio.conf, apps/, appdata/, archivos/, fondos/, espejo del
# inicio...) no esta en el payload y no se toca; la base de datos de musica tampoco.
# Rutas reemplazables con HIBYOS_* solo para la simulacion en el PC (tools/simtest_install.sh).
#
# Files that are an edited copy of a file of the official firmware (screen layouts) are not
# in the payload: derive/ holds only our changes and the file is rebuilt here from the copy
# in the player's own firmware, then checked against its SHA-256 (see "derived files" below
# and docs/INSTALLER.md).

HM=${HIBYOS_HM:-/usr/data/hibymod}
DATA=${HIBYOS_DATA:-/usr/data}
SDM=${HIBYOS_SD:-/usr/data/mnt/sd_0}
SD_RE=${HIBYOS_SD_RE:-" (/usr)?/data/mnt/sd_0 "}
RES=${HIBYOS_RES:-/usr/resource}
MOUNTS=${HIBYOS_MOUNTS:-/proc/mounts}
ROOTFS=${HIBYOS_ROOTFS:-/}
TMPD=${HIBYOS_TMP:-/tmp/hibymod}
WAIT=${HIBYOS_WAIT:-15}
FLAGS=${HIBYOS_FLAGS:-/data}
SYSD=$HM/sys
ROOTBIN=${HIBYOS_ROOTBIN:-/usr/bin}

PL=$SDM/ByOS/payload
LOG=$SDM/ByOS/install.log
TH=$HM/theme
ALLSUBS="layout/theme2 layout/theme3 litegui/theme2 litegui/theme3"
PRISTINE=$TMPD/rootfs_ro
DRV=$TMPD/derive

sd_ok() { grep -qE "$SD_RE" $MOUNTS 2>/dev/null; }

# The project was called HiByOS: its SD folder becomes ByOS. Entries that already exist in
# ByOS win; an old payload or cache is dropped (rebuilt), anything else is left in HiByOS.
sd_migrate() {
    O=$SDM/HiByOS
    [ -d $O ] || return 0
    if [ ! -e $SDM/ByOS ]; then mv $O $SDM/ByOS 2>/dev/null; return 0; fi
    for e in $O/* $O/.[!.]*; do
        [ -e "$e" ] || continue
        b=${e##*/}
        if [ ! -e "$SDM/ByOS/$b" ]; then mv "$e" "$SDM/ByOS/$b"
        else case $b in payload|cache|install.log|ota) rm -rf "$e";; esac
        fi
    done
    rmdir $O 2>/dev/null
    return 0
}

# ------------------------------------------------------------------ sustitutos (sys/)
# Los scripts del rootfs solo cambian reflasheando. Con las actualizaciones por Internet
# (docs/OTA.md) el payload puede traer una version nueva en sys/<nombre>; el script del
# rootfs la usa solo si: no es modo seguro, no hay $FLAGS/.hibymod_nosys, hay menos de 2
# arranques fallidos seguidos, y su sha256 es el de sys/MANIFEST.sha256 (que viene en el mismo
# payload verificado), y su revision (linea "# HIBYOS_SYS_REV=N") es mayor que la del rootfs:
# un payload viejo nunca sustituye a un firmware mas nuevo. Si no: el del rootfs.
sys_rev() { r=$(sed -n 's/^# HIBYOS_SYS_REV=\([0-9][0-9]*\).*/\1/p' "$1" 2>/dev/null | head -n 1); echo ${r:-0}; }
sys_ok()
{
    [ -z "$HIBYOS_NO_OVERRIDE" ] || return 1
    if [ -f $FLAGS/.hibymod_safe ] || [ -f $FLAGS/.hibymod_nosys ]; then return 1; fi
    bf=$(cat $FLAGS/.hibymod_bootfail 2>/dev/null)
    case "$bf" in ''|*[!0-9]*) bf=0 ;; esac
    [ $bf -lt 2 ] || return 1
    [ -f "$SYSD/$1" ] && [ -f $SYSD/MANIFEST.sha256 ] || return 1
    want=$(awk -v n="$1" '$2 == n { print $1; exit }' $SYSD/MANIFEST.sha256 2>/dev/null)
    [ ${#want} = 64 ] || return 1
    [ "$(sha256sum < "$SYSD/$1" 2>/dev/null | cut -d' ' -f1)" = "$want" ] || return 1
    [ "$(sys_rev "$SYSD/$1")" -gt "$(sys_rev "$ROOTBIN/$1")" ]
}
# este mismo script: si hay uno mas nuevo y valido instalado, se usa ese
case "$1" in
  boot|late|install)
    if [ "$HIBYOS_SELF" != sys ] && sys_ok hibyos_install.sh; then
        export HIBYOS_SELF=sys
        exec /bin/sh $SYSD/hibyos_install.sh "$@"
    fi ;;
esac

log()
{
    m="$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null) $*"
    [ -t 1 ] && echo "$*"
    mkdir -p $TMPD 2>/dev/null; echo "$m" >> $TMPD/install.txt 2>/dev/null
    [ -d $SDM/ByOS ] && echo "$m" >> $LOG 2>/dev/null
}

ver_of() { [ -f "$1" ] && head -n 1 "$1" 2>/dev/null | tr -d '\r\n\t '; }
installed_ver() { ver_of $HM/payload_version; }
sd_ver() { ver_of $PL/VERSION; }

nfiles() { find "$1" -type f 2>/dev/null | wc -l; }
mounted() { grep -q " $1 " $MOUNTS 2>/dev/null; }

# copia atomica: origen destino [modo]. No reescribe lo que ya es identico (ahorra flash).
put()
{
    if [ -f "$2" ] && cmp -s "$1" "$2"; then
        [ -z "$3" ] || chmod $3 "$2"
        return 0
    fi
    mkdir -p "$(dirname "$2")" || return 1
    if cp "$1" "$2.new" && cmp -s "$1" "$2.new" && { [ -z "$3" ] || chmod $3 "$2.new"; } && mv -f "$2.new" "$2"; then
        return 0
    fi
    rm -f "$2.new"
    return 1
}

# ------------------------------------------------------------------ tema de fabrica
# Copia limpia de /usr/resource: si el tema ya esta montado encima (instalacion "late" o a
# mano), se lee por un montaje bind de / (no recursivo: sin los montajes de encima).
STOCK=
stock_open()
{
    STOCK=$RES
    for s in $ALLSUBS; do
        mounted $RES/$s || continue
        mkdir -p $PRISTINE && mount -o bind $ROOTFS $PRISTINE || { log "ERROR: no puedo leer el tema de fabrica"; return 1; }
        STOCK=$PRISTINE/${RES#/}
        break
    done
    [ -d $STOCK/layout/theme3 ] || { log "ERROR: falta $STOCK/layout/theme3"; return 1; }
}
stock_close() { [ "$STOCK" = "$RES" ] || umount $PRISTINE 2>/dev/null; STOCK=; }

# completa $TH/<sub> con lo que falte de fabrica (sin pisar lo que haya)
fill_missing()
{
    (cd $STOCK/$1 && find . -type f) | while read -r f; do
        [ -e "$TH/$1/$f" ] && continue
        mkdir -p "$(dirname "$TH/$1/$f")" && cp -p "$STOCK/$1/$f" "$TH/$1/$f" || exit 1
    done
}

# $TH/<sub> = copia completa de fabrica (marca .hibyos_complete cuando se ha comprobado)
theme_base()
{
    d=$TH/$1
    [ -f $d/.hibyos_complete ] && return 0
    if [ ! -d $d ]; then
        mkdir -p $d && cp -a $STOCK/$1/. $d/ || return 1
    else
        fill_missing $1 || return 1
    fi
    [ "$(nfiles $d)" -ge "$(nfiles $STOCK/$1)" ] || return 1
    touch $d/.hibyos_complete
}

# Un $TH/<sub> incompleto que no es del payload (p. ej. theme2 con solo nuestros ficheros)
# taparia entero el de fabrica al montarlo: se quita. Barato si ya esta comprobado.
clean_partial()
{
    for s in $ALLSUBS; do
        d=$TH/$s
        [ -d $d ] && [ ! -f $d/.hibyos_complete ] || continue
        mounted $RES/$s && continue
        [ -d $RES/$s ] || continue
        if [ "$(nfiles $d)" -ge "$(nfiles $RES/$s)" ]; then
            touch $d/.hibyos_complete
        else
            case " $(cat $HM/payload_subs 2>/dev/null) " in
                *" $s "*) continue ;;          # del payload: lo completa la instalacion
            esac
            rm -rf $d && log "tema: $s incompleto, quitado (se usa el de fabrica)"
        fi
    done
}

# ------------------------------------------------------------------ inicio espejo
# El inicio de fabrica pasa a ser un espejo del launcher cuando hibyshell ha generado
# launcher/mirror.png (en el primer arranque aun no existe: se queda el inicio normal).
MIRROR_PNG=litegui/theme3/launcher/mirror.png
mirror_apply()
{
    [ -f $TH/$MIRROR_PNG ] && [ -d $HM/payload_mirror ] || return 0
    (cd $HM/payload_mirror && find . -type f) | while read -r f; do
        put "$HM/payload_mirror/$f" "$TH/$f" || exit 1
    done || return 1
    touch $HM/mirror_on
    log "inicio espejo activado"
}

# ------------------------------------------------------------------ derived files (derive/)
# The payload carries no text of the official firmware. A file that is an edited copy of an
# official one comes as a recipe: derive/INDEX has one line per accepted starting file
#     <req|opt> <payload path> <sha256 of the result> <base> <sha256 of the base> <strip> <id>
# where <base> is a path inside the firmware (usr/resource/... or usr/bin/...), <strip> = 1
# removes every CR from it first, and derive/<id>.ops lists the pieces of the result:
#     c <offset> <length>   bytes of the base      a <offset> <length>   bytes of derive/<id>.add
# (<id> "-" = the base already is the result). The first line whose base has the stated
# SHA-256 is used, and the result must have the stated SHA-256 or it is thrown away.
# Nothing is written outside $DRV here. A "req" file that cannot be built stops the
# installation before anything is changed; an "opt" one is skipped and logged, and the
# firmware's own file stays in use.
sha_of() { sha256sum < "$1" 2>/dev/null | cut -d' ' -f1; }
src_of() { if [ -f "$DRV/files/$1" ]; then echo "$DRV/files/$1"; else echo "$PL/$1"; fi; }
drv_base()
{
    case "$1" in
        usr/resource/*) echo "$STOCK/${1#usr/resource/}" ;;
        usr/bin/*)      echo "$ROOTBIN/${1#usr/bin/}" ;;
        *)              echo "" ;;
    esac
}

# base strip id sha256 -> $DRV/out.tmp (0 only if it has that sha256)
derive_one()
{
    if [ "$3" = - ]; then
        cp "$1" $DRV/out.tmp && [ "$(sha_of $DRV/out.tmp)" = "$4" ]
        return $?
    fi
    ops=$PL/derive/$3.ops; add=$PL/derive/$3.add
    [ -f $ops ] && [ -f $add ] || return 1
    src=$1
    if [ "$2" = 1 ]; then
        tr -d '\r' < "$1" > $DRV/base.tmp || return 1
        src=$DRV/base.tmp
    fi
    # whole files in memory (they are small texts with no byte 1), pieces by position
    LC_ALL=C awk -v B="$src" -v A="$add" '
        BEGIN { RS = "\001"; getline b < B; getline a < A; RS = "\n" }
        $1 == "c" { printf "%s", substr(b, $2 + 1, $3) }
        $1 == "a" { printf "%s", substr(a, $2 + 1, $3) }' $ops > $DRV/out.tmp 2>/dev/null
    [ "$(sha_of $DRV/out.tmp)" = "$4" ] && return 0
    # same thing byte by byte with dd (slow; only if this awk did not do it right)
    while read -r k o n; do
        case "$k" in
            c) dd if="$src" bs=1 skip=$o count=$n 2>/dev/null ;;
            a) dd if="$add" bs=1 skip=$o count=$n 2>/dev/null ;;
        esac
    done < $ops > $DRV/out.tmp
    [ "$(sha_of $DRV/out.tmp)" = "$4" ]
}

# builds every derived file in $DRV/files/<payload path>; $DRV/list = their MANIFEST-like lines
derive_prepare()
{
    rm -rf $DRV; mkdir -p $DRV/files || return 1
    : > $DRV/list; : > $DRV/seen
    [ -f $PL/derive/INDEX ] || return 0
    while read -r kind dest want base bsum strip id rest; do
        case "$kind" in req|opt) ;; *) continue ;; esac
        [ -n "$id" ] && [ -n "$(dest_of "$dest")" ] || continue
        case "$dest $base $id" in *..*) continue ;; esac
        [ -f "$PL/$dest" ] || [ -f "$DRV/files/$dest" ] && continue
        b=$(drv_base "$base")
        [ -n "$b" ] && [ -f "$b" ] && [ "$(sha_of "$b")" = "$bsum" ] || continue
        if derive_one "$b" "$strip" "$id" "$want" && mkdir -p "$(dirname "$DRV/files/$dest")" && mv -f $DRV/out.tmp "$DRV/files/$dest"; then
            echo "derived  $dest" >> $DRV/list
        else
            log "derive: $dest: recipe $id did not give the expected file"
        fi
    done < $PL/derive/INDEX
    # what no line could build: the copy already installed if it is the right one; if not,
    # "req" stops the installation and "opt" is left out
    bad=0
    while read -r kind dest want rest; do
        case "$kind" in req|opt) ;; *) continue ;; esac
        d=$(dest_of "$dest"); [ -n "$d" ] || continue
        case "$dest" in *..*) continue ;; esac
        [ -f "$PL/$dest" ] || [ -f "$DRV/files/$dest" ] && continue
        grep -qxF "$dest" $DRV/seen && continue
        echo "$dest" >> $DRV/seen
        if [ -f "$HM/$d" ] && [ "$(sha_of "$HM/$d")" = "$want" ] && mkdir -p "$(dirname "$DRV/files/$dest")" && cp "$HM/$d" "$DRV/files/$dest"; then
            echo "derived  $dest" >> $DRV/list
            log "derive: $dest: firmware file not recognised, the copy already installed is kept"
        elif [ "$kind" = req ]; then
            log "ERROR: derive: $dest cannot be built: the file of this firmware is not a known version"
            bad=1
        else
            log "derive: $dest skipped: the file of this firmware is not a known version (the firmware's own file stays in use)"
        fi
    done < $PL/derive/INDEX
    rm -f $DRV/out.tmp $DRV/base.tmp
    [ $bad = 0 ]
}

# ------------------------------------------------------------------ instalacion
# install_failed: version that failed + revision of the installer that failed (a newer
# installer tries again: it may know how to install what the old one could not)
MYREV=$(sys_rev "$0")
fail()
{
    log "ERROR: $*"
    { echo "$NEWV"; echo "rev $MYREV"; } > $HM/install_failed
    rm -rf $DRV
    stock_close
    sync
    return 1
}

# destino en /usr/data/hibymod de una ruta del payload ("" = no se instala ahi)
dest_of()
{
    case "$1" in
        bin/hibymod_disp.so) echo preload/hibymod_disp.so ;;
        bin/*)               echo shell/${1#bin/} ;;
        shell/*|theme/*)     echo $1 ;;
        sys/*)               echo $1 ;;
        mirror/*)            echo payload_mirror/${1#mirror/} ;;
        *)                   echo "" ;;
    esac
}

do_install()
{
    NEWV=$(sd_ver)
    OLDV=$(installed_ver)
    first=0; [ -n "$OLDV" ] || first=1
    log "instalando ByOS $NEWV (instalada: ${OLDV:-ninguna})"
    [ -f $PL/MANIFEST.md5 ] || { fail "falta MANIFEST.md5"; return 1; }
    (cd $PL && md5sum -c MANIFEST.md5 >/dev/null 2>&1) || { fail "payload danado o incompleto en la SD (vuelve a copiar la carpeta ByOS)"; return 1; }
    mkdir -p $HM || { fail "no puedo crear $HM"; return 1; }

    # derived files (derive/): built and checked in $DRV before anything is changed
    stock_open || { fail "tema de fabrica no disponible"; return 1; }
    derive_prepare || { fail "this payload does not fit the firmware of this player (see the lines above); nothing was changed"; return 1; }
    # everything to install: the files of the payload + the derived ones
    awk 'NF >= 2' $PL/MANIFEST.md5 > $DRV/all && cat $DRV/list >> $DRV/all || { fail "no puedo escribir en $TMPD"; return 1; }

    # espacio: ficheros nuevos + el mayor (copia .new) + tema de fabrica si falta + 1 MB
    need=1024; big=0
    while read -r sum f; do
        d=$(dest_of $f); [ -n "$d" ] || continue
        sz=$(( ($(wc -c < "$(src_of $f)") + 1023) / 1024 ))
        [ $sz -gt $big ] && big=$sz
        [ -e $HM/$d ] || need=$((need + sz))
    done < $DRV/all
    SUBS=$(for s in $ALLSUBS; do if [ -d $PL/theme/$s ] || [ -d $DRV/files/theme/$s ]; then printf '%s ' $s; fi; done)
    for s in $SUBS; do
        [ -d $TH/$s ] || need=$((need + $(du -sk $STOCK/$s | cut -f1)))
    done
    need=$((need + big))
    avail=$(df -k $DATA 2>/dev/null | awk 'NR==2{print $4}')
    [ "${avail:-0}" -gt $need ] || { fail "sin espacio en $DATA: hay ${avail:-?} KB, hacen falta $need KB"; return 1; }

    # a partir de aqui una instalacion cortada se repite en el siguiente arranque
    rm -f $HM/payload_version $HM/install_failed
    sync

    # lista de destinos de esta version (payload_files)
    mkdir -p $TMPD; : > $TMPD/newfiles
    while read -r sum f; do
        d=$(dest_of $f)
        case "$d" in ''|payload_mirror/*) ;; *) echo "$d" >> $TMPD/newfiles ;; esac
    done < $DRV/all

    # 1. tema: copia completa de fabrica de cada sub del payload + nuestros ficheros
    for s in $SUBS; do
        theme_base $s || { fail "no se pudo copiar el tema de fabrica $s"; return 1; }
    done
    # ficheros nuestros de la version anterior que ya no estan: vuelve el de fabrica
    if [ -f $HM/payload_files ]; then
        while read -r d; do
            grep -qx "$d" $TMPD/newfiles 2>/dev/null && continue
            case "$d" in
                theme/*) r=${d#theme/}
                         if [ -f "$STOCK/$r" ]; then cp -p "$STOCK/$r" "$TH/$r"; else rm -f "$TH/$r"; fi ;;
                shell/*|preload/*|sys/*) rm -f "$HM/$d" ;;
            esac
        done < $HM/payload_files
    fi 2>/dev/null
    stock_close

    # 2. ficheros del payload (binarios con permiso de ejecucion)
    while read -r sum f; do
        d=$(dest_of $f); [ -n "$d" ] || continue
        mode=; case "$f" in bin/hibymod_disp.so|bin/cacert.pem|sys/*) mode=644 ;; bin/*) mode=755 ;; esac
        put "$(src_of $f)" "$HM/$d" $mode || { fail "no se pudo copiar $f"; return 1; }
    done < $DRV/all
    # obsoletos del payload_mirror
    (cd $HM/payload_mirror 2>/dev/null && find . -type f) | while read -r f; do
        [ -f "$PL/mirror/$f" ] || [ -f "$DRV/files/mirror/$f" ] || rm -f "$HM/payload_mirror/$f"
    done

    # 3. inicio de fabrica sin launcher (hiby_player.sh lo usa si el launcher no arranca):
    #    iconos y vista normales del tema, tambien cuando el espejo esta activo
    rm -rf $HM/mirror_bak; mkdir -p $HM/mirror_bak
    for f in $PL/theme/litegui/theme3/launcher/*.png; do
        [ -f "$f" ] && { put "$f" $HM/mirror_bak/$(basename "$f") || { fail "mirror_bak"; return 1; }; }
    done
    put "$(src_of theme/layout/theme3/launcher/hiby_launcher_apps.view)" $HM/launcher_view.bak || { fail "launcher_view.bak"; return 1; }
    rm -f $HM/mirror_on
    mirror_apply || { fail "no se pudo aplicar el inicio espejo"; return 1; }

    # 4. capturas de apps del launcher: se regeneran (la interfaz puede haber cambiado)
    [ "$OLDV" != "$NEWV" ] && rm -rf $HM/shell/cache

    # 5. libreria de pantalla: preload_ok solo con la libreria ya en su sitio y verificada,
    #    en la primera instalacion y si no se ha desactivado (orden SIN_LAUNCHER)
    if [ $first = 1 ] && [ ! -f $DATA/.hibymod_nopreload ] && cmp -s $PL/bin/hibymod_disp.so $HM/preload/hibymod_disp.so; then
        touch $HM/preload_ok
    fi

    # 6. fondos a /Pictures de la SD (solo los que falten)
    mkdir -p $SDM/Pictures 2>/dev/null
    for f in $PL/Pictures/*; do
        [ -f "$f" ] || continue
        [ -e "$SDM/Pictures/$(basename "$f")" ] || cp "$f" "$SDM/Pictures/" 2>/dev/null
    done

    # 7. registro y version (lo ultimo)
    mv -f $TMPD/newfiles $HM/payload_files
    echo "$SUBS" > $HM/payload_subs
    rm -f $HM/install_nowait
    rm -rf $DRV
    echo "$NEWV" > $HM/payload_version
    sync
    log "ByOS $NEWV instalado ($(du -sk $HM/shell $HM/preload $TH 2>/dev/null | awk '{s+=$1} END{print s}') KB)"
    return 0
}

# ha que instalar? (version de la SD distinta y no fallida ya con esa misma version)
pending()
{
    v=$(sd_ver)
    [ -n "$v" ] || return 1
    [ "$v" = "$(installed_ver)" ] && return 1
    fr=$(sed -n '2s/^rev //p' $HM/install_failed 2>/dev/null)
    if [ "$v" = "$(ver_of $HM/install_failed)" ] && [ "${fr:-1}" = "$MYREV" ]; then
        [ -f $TMPD/install_failed_logged ] || { log "ByOS $v ya fallo antes: no se reintenta (hibyos_install.sh install para forzar)"; touch $TMPD/install_failed_logged; }
        return 1
    fi
    return 0
}

# ByOS is drawn for the player's dark theme (theme3). After a factory reset the player starts
# with the light one and its menus show over ByOS. user.ini is the player's settings struct as
# is (docs/research_settings_api.md); the UI theme is the 32-bit value at 0x1c (2 = dark,
# found by comparing the file before and after changing it in the stock settings).
dark_theme() {
    U=${HIBYOS_USERINI:-/usr/data/user.ini}
    [ -f $U ] || return 0
    [ "$(wc -c < $U)" -ge 32 ] || return 0
    v=$(dd if=$U bs=1 skip=28 count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
    if [ "$v" != 2 ]; then
        printf '\002' | dd of=$U bs=1 seek=28 count=1 conv=notrunc 2>/dev/null && sync
        log "tema de la interfaz: $v -> 2 (oscuro)"
    fi
    # "Theme color" tints every screen with a hue, ByOS included: always off. 32-bit flag at
    # 0x3e0 (0 off, 1 on; the hue itself is at 0x3dc and is left alone). Same method.
    [ "$(wc -c < $U)" -ge 996 ] || return 0
    c=$(dd if=$U bs=1 skip=992 count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
    if [ "$c" != 0 ]; then
        printf '\000' | dd of=$U bs=1 seek=992 count=1 conv=notrunc 2>/dev/null && sync
        log "color del tema: $c -> 0 (desactivado)"
    fi
}

case "$1" in
  boot)
    clean_partial
    dark_theme
    [ -f $TH/$MIRROR_PNG ] && [ ! -f $HM/mirror_on ] && mirror_apply
    if ! sd_ok; then
        # instalado (o sin payload la ultima vez): no se espera a la SD; si hay una version
        # nueva, la instala S95hibymod en cuanto se monte la SD (y reinicia)
        [ -n "$(installed_ver)" ] || [ -f $HM/install_nowait ] && exit 0
        i=0
        while [ $i -lt $((WAIT * 2)) ] && ! sd_ok; do sleep 0.5; i=$((i + 1)); done
        if ! sd_ok; then
            mkdir -p $HM && touch $HM/install_nowait
            log "sin SD tras ${WAIT} s: ByOS no instalado (se instalara al montarse la SD)"
            exit 0
        fi
    fi
    sd_migrate
    if pending; then
        do_install
    elif [ -z "$(installed_ver)" ] && [ ! -f $PL/VERSION ]; then
        mkdir -p $HM && [ ! -f $HM/install_nowait ] && touch $HM/install_nowait && log "no hay ByOS/payload en la SD"
    fi
    exit 0
    ;;
  late)
    sd_ok || exit 1
    sd_migrate
    pending || exit 1
    do_install || exit 1
    exit 0
    ;;
  install)
    sd_ok || { echo "SD no montada"; exit 1; }
    sd_migrate
    [ -f $PL/VERSION ] || { echo "no hay $PL/VERSION"; exit 1; }
    rm -f $HM/install_failed
    do_install || exit 1
    echo "reinicia para usar ByOS $(installed_ver)"
    ;;
  sysok)
    sys_ok "$2"
    exit $?
    ;;
  status)
    echo "instalada: $(installed_ver || true)"
    sd_ok && echo "en la SD: $(sd_ver)" || echo "SD no montada"
    [ -f $HM/install_failed ] && echo "fallo con: $(cat $HM/install_failed)"
    [ -f $HM/mirror_on ] && echo "inicio espejo: si" || echo "inicio espejo: no"
    tail -n 5 $LOG 2>/dev/null
    ;;
  *)
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
exit 0
