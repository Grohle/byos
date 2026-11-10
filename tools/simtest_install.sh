#!/bin/bash
# Simulacion en el PC (WSL) de overlay/usr/bin/hibyos_install.sh con dobles de prueba.
#   bash tools/simtest_install.sh            (antes: python tools/make_payload.py en Windows)
# El instalador se ejecuta con dash (sh POSIX, como el ash de busybox). Recursos de fabrica:
# ~/hiby/rootfs/usr/resource. Todo ocurre en /tmp/hisim; no toca el aparato.
#
# Environment (all optional):
#   STOCKRES   usr/resource of YOUR unpacked official firmware (default ~/hiby/rootfs/usr/resource)
#   SRC_SD     the HiByOS folder to install (default out/sd/HiByOS, from tools/make_payload.py)
#   EXPECT     folder written by "make_payload.py --expect": the full files the player must
#              end up with for everything in payload/derive/ (compared byte by byte if present;
#              default out/sd_expect)
#   SIM_BB     command that runs the player's own busybox, e.g.
#              "qemu-mipsel-static -L ROOTFS ROOTFS/bin/busybox": the installer and every tool
#              it calls (awk, dd, sha256sum, cp...) then run with that busybox, not the PC's
# The official files that HiByOS edits are rebuilt from STOCKRES with tools/vendor_edits.py
# into /tmp/hisim_vendor (never inside the repository).
R=$(cd "$(dirname "$0")/.." && pwd)
INST=$R/overlay/usr/bin/hibyos_install.sh
SRC_SD=${SRC_SD:-$R/out/sd/HiByOS}
STOCKRES=${STOCKRES:-$HOME/hiby/rootfs/usr/resource}
STOCKROOT=$(cd "$STOCKRES/../.." && pwd)
EXPECT=${EXPECT:-$R/out/sd_expect}
T=/tmp/hisim
VEND=/tmp/hisim_vendor
SH=dash
[ -z "$SIM_BB" ] || SH="$SIM_BB sh"
fails=0
ok()  { echo "  ok   $*"; }
bad() { echo "  FALLO $*"; fails=$((fails + 1)); }
chk() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

[ -f $SRC_SD/payload/VERSION ] || { echo "falta $SRC_SD (python tools/make_payload.py)"; exit 1; }
# the official files with the HiByOS edits, as the patcher leaves them in the firmware
rm -rf $VEND
python3 $R/tools/vendor_edits.py export --stock $STOCKROOT --out $VEND > $VEND.log 2>&1 || { cat $VEND.log; echo "vendor_edits.py export failed (is STOCKRES the official v1.3 firmware?)"; exit 1; }
for f in $INST $R/overlay/etc/init.d/S95hibymod $VEND/usr/bin/hiby_player.sh $VEND/etc/init.d/S11jpeg_display_shell; do
    dash -n $f && bash -n $f || { echo "SINTAXIS MAL: $f"; exit 1; }
done
echo "sintaxis ok (dash -n, bash -n)"

reset_dev()
{
    rm -rf $T; mkdir -p $T/bin $T/data/hibymod $T/sd $T/tmp $T/res $T/rootbin
    # recursos "del rootfs": fabrica + nuestros ficheros del overlay + los ficheros oficiales
    # editados por el parcheador (como en el .upt). "reset_dev stock": without those edits.
    for s in layout/theme2 layout/theme3 litegui/theme2 litegui/theme3; do
        mkdir -p $T/res/$s; cp -a $STOCKRES/$s/. $T/res/$s/
        [ -d $R/overlay/usr/resource/$s ] && cp -a $R/overlay/usr/resource/$s/. $T/res/$s/
        [ "$1" != stock ] && [ -d $VEND/usr/resource/$s ] && cp -a $VEND/usr/resource/$s/. $T/res/$s/
    done
    # /usr/bin of the firmware: the installer itself (what hiby_player.sh and S95hibymod call)
    cp ${2:-$INST} $T/rootbin/hibyos_install.sh
    # vista "de solo lectura" de / (montaje bind sin los montajes de encima)
    mkdir -p $T/proot$T; cp -a $T/res $T/proot$T/res
    : > $T/mounts
    cat > $T/bin/mount <<X
#!/bin/sh
# mount -o bind ORIGEN DESTINO -> enlace simbolico
[ "\$1" = "-o" ] && [ "\$2" = bind ] || exit 1
rmdir "\$4" 2>/dev/null; ln -sfn "\$3" "\$4"; echo "\$*" >> $T/mount.log
X
    printf '#!/bin/sh\nrm -f "$1"; mkdir -p "$1"\n' > $T/bin/umount
    printf '#!/bin/sh\nexit 0\n' > $T/bin/sync
    # with SIM_BB every tool the installer calls is the player's busybox
    if [ -n "$SIM_BB" ]; then
        for a in awk cat chmod cmp cp cut date dd df dirname du find grep head md5sum mkdir mv rm sed sha256sum sleep tail touch tr wc; do
            printf '#!/bin/sh\nexec %s %s "$@"\n' "$SIM_BB" $a > $T/bin/$a
        done
    fi
    chmod +x $T/bin/*
}
sd_in()  { echo "/dev/mmcblk0 $T/sd exfat rw 0 0" >> $T/mounts; }
sd_out() { sed -i "\# $T/sd #d" $T/mounts; }
put_payload() { rm -rf $T/sd/HiByOS; cp -a ${1:-$SRC_SD} $T/sd/HiByOS; }
remanifest() { (cd $1 && find . -type f ! -name MANIFEST.md5 | sed 's#^\./##' | sort | xargs md5sum > MANIFEST.md5); }

PCPATH=$PATH
export HIBYOS_HM=$T/data/hibymod HIBYOS_DATA=$T/data HIBYOS_SD=$T/sd \
    HIBYOS_SD_RE=" $T/sd " HIBYOS_RES=$T/res HIBYOS_MOUNTS=$T/mounts HIBYOS_ROOTFS=$T/proot \
    HIBYOS_TMP=$T/tmp HIBYOS_WAIT=2 HIBYOS_ROOTBIN=$T/rootbin
# the firmware's copy of the installer (it hands over to a newer valid one in sys/, docs/OTA.md);
# the test doubles (mount, umount, sync) and SIM_BB tools are only in the installer's PATH
run() { PATH=$T/bin:$PCPATH $SH $T/rootbin/hibyos_install.sh "$@"; }
H=$T/data/hibymod
PL=$SRC_SD/payload
V=$(cat $PL/VERSION)
LIMIT=1000; [ -z "$SIM_BB" ] || LIMIT=8000      # "no work" limit in ms (an emulated busybox is slow)
md() { md5sum < "$1" | cut -d' ' -f1; }
sha() { sha256sum < "$1" | cut -d' ' -f1; }
snap() { (cd $T/data && find . -type f -newer $T/stamp | sort); }
# same PAYLOAD_PATH INSTALLED_FILE: the installed file is the one that payload path stands
# for: shipped in full, or (payload/derive/) the file whose SHA-256 derive/INDEX states and,
# if make_payload.py --expect was used, byte for byte the builder's full file
want_sha() { awk -v d="$1" '($1 == "req" || $1 == "opt") && $2 == d { print $3; exit }' $PL/derive/INDEX 2>/dev/null; }
derived() { awk '$1 == "req" || $1 == "opt" { print $2 }' $PL/derive/INDEX 2>/dev/null | sort -u; }
same()
{
    if [ -f "$PL/$1" ]; then cmp -s "$PL/$1" "$2"; return; fi
    w=$(want_sha "$1")
    [ -n "$w" ] && [ -f "$2" ] && [ "$(sha "$2")" = "$w" ] || return 1
    [ ! -f "$EXPECT/$1" ] || cmp -s "$EXPECT/$1" "$2"
}
if [ -f $PL/derive/INDEX ]; then
    echo "payload with derive/: $(derived | wc -l) files are rebuilt on the player$([ -d "$EXPECT" ] && echo " (compared with $EXPECT)")"
fi

echo; echo "=== 1. instalacion desde cero (SD montada antes del reproductor)"
reset_dev; sd_in; put_payload
run boot; rc=$?
chk "sale con 0" "[ $rc = 0 ]"
chk "payload_version = $V" "[ \"\$(cat $H/payload_version)\" = $V ]"
chk "hibyshell ejecutable e identico" "[ -x $H/shell/hibyshell ] && cmp -s $PL/bin/hibyshell $H/shell/hibyshell"
chk "hibywall/hibymix/hibytag ejecutables" "[ -x $H/shell/hibywall ] && [ -x $H/shell/hibymix ] && [ -x $H/shell/hibytag ]"
chk "libreria en preload/ y preload_ok creado" "cmp -s $PL/bin/hibymod_disp.so $H/preload/hibymod_disp.so && [ -f $H/preload_ok ]"
chk "capas e iconos del shell" "[ -f $H/shell/home_btn.ov ] && [ -f $H/shell/icons/music_56.ov ] && [ -f $H/shell/wall.raw ]"
n_st=$(find $STOCKRES/layout/theme3 $STOCKRES/litegui/theme3 -type f | wc -l)
n_th=$(find $H/theme/layout/theme3 $H/theme/litegui/theme3 -type f ! -name .hibyos_complete | wc -l)
chk "tema completo ($n_th ficheros >= $n_st de fabrica)" "[ $n_th -ge $n_st ]"
chk "nuestros ficheros encima (volume.dlg, topbar.view, launcher)" "same theme/layout/theme3/dialog/volume.dlg $H/theme/layout/theme3/dialog/volume.dlg && same theme/layout/theme3/topbar/topbar.view $H/theme/layout/theme3/topbar/topbar.view && cmp -s $PL/theme/litegui/theme3/launcher/music.png $H/theme/litegui/theme3/launcher/music.png"
chk "about screens (hiby_about_dev.view)" "same theme/layout/theme3/hiby_about_dev.view $H/theme/layout/theme3/hiby_about_dev.view && same theme/litegui/theme3/hiby_about_dev.view $H/theme/litegui/theme3/hiby_about_dev.view"
chk "resto de fabrica intacto (hiby_main_category.view)" "cmp -s $STOCKRES/layout/theme3/hiby_main_category.view $H/theme/layout/theme3/hiby_main_category.view"
chk "sin theme2 en /usr/data (no se tapa el de fabrica)" "[ ! -d $H/theme/layout/theme2 ] && [ ! -d $H/theme/litegui/theme2 ]"
chk "espejo NO activo aun (falta mirror.png)" "[ ! -f $H/mirror_on ] && same theme/layout/theme3/launcher/hiby_launcher_apps.view $H/theme/layout/theme3/launcher/hiby_launcher_apps.view"
chk "mirror_bak + launcher_view.bak + payload_mirror" "[ -f $H/mirror_bak/music.png ] && same theme/layout/theme3/launcher/hiby_launcher_apps.view $H/launcher_view.bak && same mirror/layout/theme3/launcher/hiby_launcher_apps.view $H/payload_mirror/layout/theme3/launcher/hiby_launcher_apps.view"
chk "payload_subs = layout/theme3 litegui/theme3" "[ \"\$(cat $H/payload_subs)\" = 'layout/theme3 litegui/theme3 ' ]"
chk "no work files left in the temporary folder" "[ ! -e $T/tmp/derive ]"
chk "9 fondos en /Pictures de la SD" "[ \$(ls $T/sd/Pictures | wc -l) = 9 ]"
chk "install.log en la SD" "grep -q 'instalado' $T/sd/HiByOS/install.log"
chk "nada de estado de usuario creado (launcher.conf)" "[ ! -f $H/shell/launcher.conf ]"
grep instalado $T/sd/HiByOS/install.log | tail -1 | sed 's/^/       /'

echo; echo "=== 2. misma version: no hace nada"
touch $T/stamp; sleep 1
t0=$(date +%s%N); run boot; t1=$(date +%s%N)
chk "boot sin escrituras en /usr/data" "[ -z \"\$(snap)\" ]"
echo "       boot: $(( (t1 - t0) / 1000000 )) ms en el PC"
run late; rc=$?
chk "late sale con 1 (no reinicia)" "[ $rc = 1 ] && [ -z \"\$(snap)\" ]"

echo; echo "=== 3. sin SD (instalado): ni espera ni escribe"
sd_out
t0=$(date +%s%N); run boot; t1=$(date +%s%N)
chk "boot inmediato (<1 s) y sin escrituras" "[ $(( (t1 - t0) / 1000000 )) -lt $LIMIT ] && [ -z \"\$(snap)\" ]"
run late; chk "late sale con 1" "[ $? = 1 ]"

echo; echo "=== 4. el shell genera mirror.png -> el siguiente arranque activa el espejo"
printf 'PNG' > $H/theme/litegui/theme3/launcher/mirror.png
run boot
chk "mirror_on y vista espejo" "[ -f $H/mirror_on ] && same mirror/layout/theme3/launcher/hiby_launcher_apps.view $H/theme/layout/theme3/launcher/hiby_launcher_apps.view"
chk "iconos transparentes en el tema" "cmp -s $PL/mirror/litegui/theme3/launcher/music.png $H/theme/litegui/theme3/launcher/music.png"
touch $T/stamp; sleep 1; run boot
chk "siguiente arranque: sin escrituras" "[ -z \"\$(snap)\" ]"

echo; echo "=== 5. actualizacion (S95 'late', tema montado) conservando el estado del usuario"
# estado del usuario
printf 'wall /usr/data/mnt/sd_0/Pictures/Brasa.png\n' > $H/shell/launcher.conf
printf 'x' > $H/shell/wall_custom.raw; printf 'boot\n1 2 3 4 5 26\n' > $H/shell/mirror.sig
mkdir -p $H/shell/cache $H/apps/org.x $H/appdata/org.x $H/archivos $H/fondos
printf 'a' > $H/shell/cache/music.raw; printf 'app' > $H/apps/org.x/app.json
echo 'cpu 1' > $H/power.conf; echo 'eq 1' > $H/audio.conf; printf 'f' > $H/fondos/mio.png
printf 'MIO' > "$T/sd/Pictures/Aurora.png"; mtx=$(md $H/theme/litegui/theme3/launcher/mirror.png)
# version nueva: cambia una capa, quita una capa y un fichero del tema, anade otro
P2=$T/p2; rm -rf $P2; cp -a $SRC_SD $P2
echo 9.9.9 > $P2/payload/VERSION
printf 'NUEVO' > $P2/payload/shell/home_bar.ov
rm $P2/payload/shell/cc_wave.ov $P2/payload/theme/litegui/theme3/topbar/adb.png $P2/payload/theme/litegui/theme3/launcher/dock.png
printf 'X' > $H/theme/litegui/theme3/topbar/adb.png   # instalado != rootfs
printf 'X' > $P2/payload/theme/litegui/theme3/launcher/extra.png
remanifest $P2/payload
sd_in; put_payload $P2
# tema montado sobre /usr/resource (como tras hiby_player.sh): leer /usr/resource daria el tema
for s in layout/theme3 litegui/theme3; do echo "ubi1 $T/res/$s ubifs rw 0 0" >> $T/mounts; rm -rf $T/res/$s; ln -s $H/theme/$s $T/res/$s; done
run late; rc=$?
chk "late sale con 0 (S95 reinicia)" "[ $rc = 0 ] && [ \"\$(cat $H/payload_version)\" = 9.9.9 ]"
chk "capa cambiada" "[ \"\$(cat $H/shell/home_bar.ov)\" = NUEVO ]"
chk "capa quitada se borra" "[ ! -f $H/shell/cc_wave.ov ]"
chk "fichero del tema quitado vuelve al del rootfs (topbar/adb.png)" "cmp -s $T/proot$T/res/litegui/theme3/topbar/adb.png $H/theme/litegui/theme3/topbar/adb.png"
chk "fichero del tema quitado que no es de fabrica se borra (dock.png)" "[ ! -f $H/theme/litegui/theme3/launcher/dock.png ]"
chk "fichero nuevo del tema" "[ -f $H/theme/litegui/theme3/launcher/extra.png ]"
chk "se leyo la copia de fabrica limpia (montaje bind de /)" "grep -q 'bind $T/proot' $T/mount.log && [ -d $T/tmp/rootfs_ro ] && [ ! -L $T/tmp/rootfs_ro ]"
chk "launcher.conf, wall_custom.raw, mirror.sig intactos" "grep -q Brasa $H/shell/launcher.conf && [ \"\$(cat $H/shell/wall_custom.raw)\" = x ] && grep -q '1 2 3' $H/shell/mirror.sig"
chk "power.conf, audio.conf, apps/, appdata/, archivos/, fondos/ intactos" "[ \"\$(cat $H/power.conf)\" = 'cpu 1' ] && [ \"\$(cat $H/audio.conf)\" = 'eq 1' ] && [ -f $H/apps/org.x/app.json ] && [ -d $H/appdata/org.x ] && [ -d $H/archivos ] && [ -f $H/fondos/mio.png ]"
chk "mirror.png del usuario intacto y espejo re-aplicado" "[ \"\$(md $H/theme/litegui/theme3/launcher/mirror.png)\" = $mtx ] && [ -f $H/mirror_on ] && cmp -s $PL/mirror/litegui/theme3/launcher/music.png $H/theme/litegui/theme3/launcher/music.png"
chk "capturas de apps (cache) borradas (se regeneran)" "[ ! -d $H/shell/cache ]"
chk "fondo de la SD del usuario no se pisa" "[ \"\$(cat $T/sd/Pictures/Aurora.png)\" = MIO ]"
chk "preload_ok se mantiene" "[ -f $H/preload_ok ]"
run late; chk "otra vez late: nada (1)" "[ $? = 1 ]"

echo; echo "=== 6. payload danado: no toca nada y no reintenta en bucle"
P3=$T/p3; rm -rf $P3; cp -a $P2 $P3; echo 9.9.10 > $P3/payload/VERSION; remanifest $P3/payload
printf 'corrupto' >> $P3/payload/bin/hibyshell
put_payload $P3; touch $T/stamp; sleep 1
run late; rc=$?
chk "late sale con 1, sigue 9.9.9" "[ $rc = 1 ] && [ \"\$(cat $H/payload_version)\" = 9.9.9 ]"
chk "solo escribe install_failed" "[ \"\$(snap)\" = './hibymod/install_failed' ]"
run late; chk "no reintenta la misma version" "[ $? = 1 ]"
grep -E 'ERROR|no se reintenta' $T/sd/HiByOS/install.log | tail -2 | sed 's/^/       /'

echo; echo "=== 7. aparato sin SD en el primer arranque -> espera $HIBYOS_WAIT s una vez; S95 instala al montarse"
reset_dev
t0=$(date +%s); run boot; t1=$(date +%s)
chk "espero ~${HIBYOS_WAIT} s y marco install_nowait" "[ $((t1 - t0)) -ge $HIBYOS_WAIT ] && [ -f $H/install_nowait ] && [ ! -f $H/payload_version ]"
t0=$(date +%s%N); run boot; t1=$(date +%s%N)
chk "siguiente arranque sin SD: no espera" "[ $(( (t1 - t0) / 1000000 )) -lt $LIMIT ]"
sd_in; put_payload
run late; rc=$?
chk "SD montada: late instala (0) y quita install_nowait" "[ $rc = 0 ] && [ \"\$(cat $H/payload_version)\" = $V ] && [ ! -f $H/install_nowait ] && [ -f $H/preload_ok ]"

echo; echo "=== 8. SD sin payload en el primer arranque; SIN_LAUNCHER respetado"
reset_dev; sd_in; touch $T/data/.hibymod_nopreload
run boot
chk "sin payload: install_nowait, nada instalado" "[ -f $H/install_nowait ] && [ ! -f $H/payload_version ]"
put_payload; run late
chk "instala pero sin preload_ok (.hibymod_nopreload)" "[ -f $H/payload_version ] && [ ! -f $H/preload_ok ]"

echo; echo "=== 9. migracion del aparato de desarrollo (sin payload_version, theme2 a medias, espejo a mano)"
reset_dev; sd_in; put_payload
mkdir -p $H/theme/layout $H/theme/litegui $H/shell $H/preload
cp -a $T/res/layout/theme3 $H/theme/layout/; cp -a $T/res/litegui/theme3 $H/theme/litegui/
mkdir -p $H/theme/layout/theme2/topbar $H/theme/litegui/theme2/topbar
cp $VEND/usr/resource/layout/theme2/topbar/topbar.view $H/theme/layout/theme2/topbar/
cp $PL/mirror/litegui/theme3/launcher/*.png $H/theme/litegui/theme3/launcher/
printf 'PNG' > $H/theme/litegui/theme3/launcher/mirror.png; touch $H/preload_ok
printf 'icon 56\n' > $H/shell/launcher.conf; printf 'viejo' > $H/shell/hibyshell
run boot
chk "instalado, theme2 a medias quitado" "[ \"\$(cat $H/payload_version)\" = $V ] && [ ! -d $H/theme/layout/theme2 ] && [ ! -d $H/theme/litegui/theme2 ]"
chk "binario nuevo, launcher.conf conservado, espejo activo" "cmp -s $PL/bin/hibyshell $H/shell/hibyshell && grep -q 'icon 56' $H/shell/launcher.conf && [ -f $H/mirror_on ]"

# ---------------------------------------------------------------- payload/derive/
# The files that are HiBy's layouts with our changes are not in the payload: the installer
# rebuilds them from the firmware's own copy (docs/INSTALLER.md, "Derived files").
all_derived_ok() { for p in $(derived); do same $p $H/$(echo $p | sed 's#^mirror/#payload_mirror/#') || { echo "       differs: $p"; return 1; }; done; }
# a text file of the firmware that is not any known version
alter() { for d in $T/res $T/proot$T/res; do printf '\n<!-- changed -->\n' >> $d/$1; done; }
if [ -f $PL/derive/INDEX ]; then

echo; echo "=== 10. firmware with the untouched official layouts (CRLF files): same result"
reset_dev stock; sd_in; put_payload
run boot
chk "installed" "[ \"\$(cat $H/payload_version 2>/dev/null)\" = $V ]"
chk "every derived file is the expected one" "all_derived_ok"

echo; echo "=== 11. an optional layout of the firmware is not a known version: skipped, the rest installs"
reset_dev; alter layout/theme3/topbar/topbar.view; sd_in; put_payload
run boot
chk "installed" "[ \"\$(cat $H/payload_version 2>/dev/null)\" = $V ]"
chk "topbar.view: the firmware's own file stays" "cmp -s $T/proot$T/res/layout/theme3/topbar/topbar.view $H/theme/layout/theme3/topbar/topbar.view"
chk "it is logged and not listed as ours" "grep -q 'derive: theme/layout/theme3/topbar/topbar.view skipped' $T/sd/HiByOS/install.log && ! grep -qx theme/layout/theme3/topbar/topbar.view $H/payload_files"
chk "the other derived files are right" "same theme/layout/theme3/launcher/hiby_launcher_apps.view $H/theme/layout/theme3/launcher/hiby_launcher_apps.view && same theme/layout/theme3/dialog/volume.dlg $H/theme/layout/theme3/dialog/volume.dlg && same theme/layout/theme3/hiby_about_dev.view $H/theme/layout/theme3/hiby_about_dev.view"
grep 'derive:' $T/sd/HiByOS/install.log | tail -1 | sed 's/^/       /'

echo; echo "=== 12. the home screen layout of the firmware is not a known version: refused, nothing changed"
reset_dev; alter layout/theme3/launcher/hiby_launcher_apps.view; sd_in; put_payload
touch $T/stamp; sleep 1
run boot; rc=$?
chk "boot goes on (0) and nothing is installed" "[ $rc = 0 ] && [ ! -f $H/payload_version ]"
chk "the only file written is install_failed" "[ \"\$(snap)\" = './hibymod/install_failed' ]"
chk "the reason is in the log" "grep -q 'ERROR: derive: theme/layout/theme3/launcher/hiby_launcher_apps.view cannot be built' $T/sd/HiByOS/install.log"
run late; chk "not tried again with the same version (1)" "[ $? = 1 ]"
grep 'ERROR' $T/sd/HiByOS/install.log | tail -3 | sed 's/^/       /'

echo; echo "=== 13. update where the firmware layout is unknown but the right file is already installed: kept"
reset_dev; sd_in; put_payload; run boot
alter layout/theme3/launcher/hiby_launcher_apps.view
P4=$T/p4; rm -rf $P4; cp -a $SRC_SD $P4; echo 9.9.11 > $P4/payload/VERSION; remanifest $P4/payload
put_payload $P4; run boot
chk "updated to 9.9.11" "[ \"\$(cat $H/payload_version 2>/dev/null)\" = 9.9.11 ]"
chk "every derived file is still the expected one" "all_derived_ok"

echo; echo "=== 14. an awk that cannot do it: the dd fallback gives the same files"
reset_dev stock; sd_in; put_payload
AWK=$(PATH=$T/bin:$PCPATH command -v awk)
[ "$AWK" = $T/bin/awk ] && { mv $T/bin/awk $T/bin/awk.real; AWK=$T/bin/awk.real; }
printf '#!/bin/sh\ncase "$*" in *getline*) exit 1 ;; esac\nexec %s "$@"\n' $AWK > $T/bin/awk; chmod +x $T/bin/awk
run boot
chk "installed" "[ \"\$(cat $H/payload_version 2>/dev/null)\" = $V ]"
chk "every derived file is the expected one" "all_derived_ok"

echo; echo "=== 15. payload in the old format (full files, no derive/) still installs"
reset_dev; sd_in; put_payload; run boot
P5=$T/p5; rm -rf $P5; cp -a $SRC_SD $P5; echo 9.9.12 > $P5/payload/VERSION
for p in $(derived); do
    mkdir -p $(dirname $P5/payload/$p); cp $H/$(echo $p | sed 's#^mirror/#payload_mirror/#') $P5/payload/$p
done
rm -rf $P5/payload/derive; remanifest $P5/payload
cp -a $P5 $T/../hisim_p5; reset_dev; mv $T/../hisim_p5 $P5; sd_in; put_payload $P5; run boot
chk "installed" "[ \"\$(cat $H/payload_version 2>/dev/null)\" = 9.9.12 ]"
chk "same files as with derive/" "all_derived_ok"

# the installer of the firmware builds made before derive/ existed (revision 1)
OLDINST=$T/../hisim_old_install.sh
if git -c safe.directory='*' -C $R show a7b945c:overlay/usr/bin/hibyos_install.sh > $OLDINST 2>/dev/null && grep -q '^# HIBYOS_SYS_REV=1$' $OLDINST; then
    echo; echo "=== 16. firmware with the revision 1 installer + this payload: installs on the second start"
    reset_dev "" $OLDINST; sd_in; put_payload
    run boot; rc=$?
    chk "first start: goes on (0), not installed yet, new installer stored in sys/" "[ $rc = 0 ] && [ ! -f $H/payload_version ] && cmp -s $INST $H/sys/hibyos_install.sh"
    run boot
    chk "second start: the installer in sys/ takes over and installs" "[ \"\$(cat $H/payload_version 2>/dev/null)\" = $V ] && [ -f $H/preload_ok ]"
    chk "every derived file is the expected one" "all_derived_ok"
else
    echo; echo "=== 16. (skipped: commit a7b945c with the revision 1 installer is not available)"
fi
rm -f $OLDINST

fi

echo
[ $fails = 0 ] && echo "TODO OK" || echo "$fails FALLOS"
exit $fails
