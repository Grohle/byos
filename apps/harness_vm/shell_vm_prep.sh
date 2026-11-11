#!/bin/bash
# shell_vm_prep.sh [SHELL_BINARY]  (run in WSL)
# Prepares a private copy of the QEMU harness for running hibyshell (docs/VM_SHELL.md):
#   $VMDIR (default ~/hiby/vm_store): run_vm.sh + own rootfs copy + own share/, so it never
#   collides with experiments in ~/hiby/vm; qemu/ and out/ (kernel) are symlinked from there.
#   $VMDIR/share/hs: hm_vmplayer, hibyinput (with touch log), the shell, its assets (ByOS SD
#   payload), hibynet + cacert.pem, the firmware font, a local test repository and SD packages.
# The guest side is guest/shell_vm.sh (copied to share/hs) started by share/autorun.sh at boot.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)            # repo root (…/hibymod)
H=$R/apps/harness_vm
BASE=${BASEVM:-$HOME/hiby/vm}
VMDIR=${VMDIR:-$HOME/hiby/vm_store}
SHELL_BIN=${1:-$R/out/agent_store/hibyshell}
Z="$HOME/hiby/venv/bin/python -m ziglang cc -mcpu=mips32r2 -O2 -static -s -Wall"

mkdir -p $VMDIR/share/hs/bin $VMDIR/logs
[ -e $VMDIR/qemu ] || ln -s $BASE/qemu $VMDIR/qemu
[ -e $VMDIR/out ] || ln -s $BASE/out $VMDIR/out
cp $H/run_vm.sh $H/hibyctl.py $VMDIR/
if [ ! -f $VMDIR/rootfs.ext4 ]; then
    echo "copying rootfs (1.2 GB, sparse)…"; cp --sparse=always $BASE/rootfs.ext4 $VMDIR/rootfs.ext4
fi

S=$VMDIR/share/hs
echo "building hm_vmplayer + hibyinput…"
$Z -target mipsel-linux-musleabihf -I$R/src -o $S/bin/hm_vmplayer $H/tools/hm_vmplayer.c -lpthread
$Z -target mipsel-linux-musleabi -msoft-float -o $S/bin/hibyinput $H/tools/hibyinput.c
cp "$SHELL_BIN" $S/bin/hibyshell

# /usr/data/hibymod/shell of the device: assets of the ByOS SD payload + hibynet + CA bundle
P=$R/out/sd/ByOS/payload
rm -rf $S/shell; mkdir -p $S/shell
cp -r $P/shell/. $S/shell/
cp $R/bin/hibynet $R/bin/cacert.pem $S/shell/
[ -f $R/bin/hibywall ] && cp $R/bin/hibywall $S/shell/   # caratulas (Libros, Musica)
mkdir -p $S/fonts
[ -f $S/fonts/default.otf ] || cp $HOME/hiby/rootfs/usr/resource/fonts/default.otf $S/fonts/
mkdir -p $S/sd/Apps; cp $R/out/apps/hola.hpk $S/sd/Apps/
python3 $H/scripts/mk_testrepo.py $S/repo $R/out/apps/hola.hpk 24
cp $H/guest/shell_vm.sh $S/shell_vm.sh
cat > $VMDIR/share/autorun.sh <<'EOF'
#!/bin/sh
# hibyshell harness (docs/VM_SHELL.md): input daemon with touch log, font, then the shell
HS=/mnt/share/hs
killall hibyinput 2>/dev/null; sleep 0.3
$HS/bin/hibyinput 5555 >/tmp/hibyinput.err 2>&1 &
sleep 1
if [ ! -f /usr/resource/fonts/default.otf ]; then mkdir -p /usr/resource/fonts; cp $HS/fonts/default.otf /usr/resource/fonts/; fi
[ -f $HS/noauto ] || sh $HS/shell_vm.sh start
EOF
chmod +x $VMDIR/share/autorun.sh $S/shell_vm.sh $S/bin/*
du -sh $S
echo "ready: cd $VMDIR && ./run_vm.sh --cpu 24Kf --port 5566   (then apps/harness_vm/vmdrive.py, docs/VM_SHELL.md)"
