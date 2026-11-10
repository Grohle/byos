#!/bin/bash
# Simulacion en el PC (WSL) de overlay/usr/bin/hibyos_install.sh con dobles de prueba.
#   bash tools/simtest_install.sh            (antes: python tools/make_payload.py en Windows)
# El instalador se ejecuta con dash (sh POSIX, como el ash de busybox). Recursos de fabrica:
# ~/hiby/rootfs/usr/resource. Todo ocurre en /tmp/hisim; no toca el aparato.
R=$(cd "$(dirname "$0")/.." && pwd)
INST=$R/overlay/usr/bin/hibyos_install.sh
SRC_SD=$R/out/sd/HiByOS
STOCKRES=${STOCKRES:-$HOME/hiby/rootfs/usr/resource}
T=/tmp/hisim
SH=dash
fails=0
ok()  { echo "  ok   $*"; }
bad() { echo "  FALLO $*"; fails=$((fails + 1)); }
chk() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

[ -f $SRC_SD/payload/VERSION ] || { echo "falta $SRC_SD (python tools/make_payload.py)"; exit 1; }
for f in $INST $R/overlay/etc/init.d/S95hibymod $R/overlay/usr/bin/hiby_player.sh; do
    dash -n $f && bash -n $f || { echo "SINTAXIS MAL: $f"; exit 1; }
done
echo "sintaxis ok (dash -n, bash -n)"

reset_dev()
{
    rm -rf $T; mkdir -p $T/bin $T/data/hibymod $T/sd $T/tmp $T/res
    # recursos "del rootfs": fabrica + nuestros ficheros del overlay (como en el .upt)
    for s in layout/theme2 layout/theme3 litegui/theme2 litegui/theme3; do
        mkdir -p $T/res/$s; cp -a $STOCKRES/$s/. $T/res/$s/
        [ -d $R/overlay/usr/resource/$s ] && cp -a $R/overlay/usr/resource/$s/. $T/res/$s/
    done
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
    chmod +x $T/bin/*
}
sd_in()  { echo "/dev/mmcblk0 $T/sd exfat rw 0 0" >> $T/mounts; }
sd_out() { sed -i "\# $T/sd #d" $T/mounts; }
put_payload() { rm -rf $T/sd/HiByOS; cp -a ${1:-$SRC_SD} $T/sd/HiByOS; }
remanifest() { (cd $1 && find . -type f ! -name MANIFEST.md5 | sed 's#^\./##' | sort | xargs md5sum > MANIFEST.md5); }

export PATH=$T/bin:$PATH HIBYOS_HM=$T/data/hibymod HIBYOS_DATA=$T/data HIBYOS_SD=$T/sd \
    HIBYOS_SD_RE=" $T/sd " HIBYOS_RES=$T/res HIBYOS_MOUNTS=$T/mounts HIBYOS_ROOTFS=$T/proot \
    HIBYOS_TMP=$T/tmp HIBYOS_WAIT=2
run() { $SH $INST "$@"; }
H=$T/data/hibymod
PL=$SRC_SD/payload
V=$(cat $PL/VERSION)
md() { md5sum < "$1" | cut -d' ' -f1; }
snap() { (cd $T/data && find . -type f -newer $T/stamp | sort); }

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
chk "nuestros ficheros encima (volume.dlg, topbar.view, launcher)" "cmp -s $PL/theme/layout/theme3/dialog/volume.dlg $H/theme/layout/theme3/dialog/volume.dlg && cmp -s $PL/theme/layout/theme3/topbar/topbar.view $H/theme/layout/theme3/topbar/topbar.view && cmp -s $PL/theme/litegui/theme3/launcher/music.png $H/theme/litegui/theme3/launcher/music.png"
chk "resto de fabrica intacto (hiby_main_category.view)" "cmp -s $STOCKRES/layout/theme3/hiby_main_category.view $H/theme/layout/theme3/hiby_main_category.view"
chk "sin theme2 en /usr/data (no se tapa el de fabrica)" "[ ! -d $H/theme/layout/theme2 ] && [ ! -d $H/theme/litegui/theme2 ]"
chk "espejo NO activo aun (falta mirror.png)" "[ ! -f $H/mirror_on ] && cmp -s $PL/theme/layout/theme3/launcher/hiby_launcher_apps.view $H/theme/layout/theme3/launcher/hiby_launcher_apps.view"
chk "mirror_bak + launcher_view.bak + payload_mirror" "[ -f $H/mirror_bak/music.png ] && [ -f $H/launcher_view.bak ] && [ -f $H/payload_mirror/layout/theme3/launcher/hiby_launcher_apps.view ]"
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
chk "boot inmediato (<1 s) y sin escrituras" "[ $(( (t1 - t0) / 1000000 )) -lt 1000 ] && [ -z \"\$(snap)\" ]"
run late; chk "late sale con 1" "[ $? = 1 ]"

echo; echo "=== 4. el shell genera mirror.png -> el siguiente arranque activa el espejo"
printf 'PNG' > $H/theme/litegui/theme3/launcher/mirror.png
run boot
chk "mirror_on y vista espejo" "[ -f $H/mirror_on ] && cmp -s $PL/mirror/layout/theme3/launcher/hiby_launcher_apps.view $H/theme/layout/theme3/launcher/hiby_launcher_apps.view"
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
chk "siguiente arranque sin SD: no espera" "[ $(( (t1 - t0) / 1000000 )) -lt 1000 ]"
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
cp $R/overlay/usr/resource/layout/theme2/topbar/topbar.view $H/theme/layout/theme2/topbar/
cp $PL/mirror/litegui/theme3/launcher/*.png $H/theme/litegui/theme3/launcher/
printf 'PNG' > $H/theme/litegui/theme3/launcher/mirror.png; touch $H/preload_ok
printf 'icon 56\n' > $H/shell/launcher.conf; printf 'viejo' > $H/shell/hibyshell
run boot
chk "instalado, theme2 a medias quitado" "[ \"\$(cat $H/payload_version)\" = $V ] && [ ! -d $H/theme/layout/theme2 ] && [ ! -d $H/theme/litegui/theme2 ]"
chk "binario nuevo, launcher.conf conservado, espejo activo" "cmp -s $PL/bin/hibyshell $H/shell/hibyshell && grep -q 'icon 56' $H/shell/launcher.conf && [ -f $H/mirror_on ]"

echo
[ $fails = 0 ] && echo "TODO OK" || echo "$fails FALLOS"
exit $fails
