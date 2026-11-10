#!/bin/sh
# shell_vm.sh start|stop|restart [nocache] : runs hibyshell in the QEMU harness (docs/VM_SHELL.md)
#   /usr/data          tmpfs 24 MB (like the device's free space), filled from /mnt/share/hs
#   /usr/data/mnt/sd_0 its own tmpfs, so st_dev differs and the store sees an "SD card"
#   hm_vmplayer        stand-in for hiby_player + hibymod_disp.so (shared state, FIFO, panel)
# Options (files in /mnt/share/hs): lang = es|en|fr (default en), repo_url = extra store repo
# (default http://10.0.2.2:8765/index.json, the host's test repo); "nocache" wipes the store cache.
HS=/mnt/share/hs
D=/usr/data/hibymod
stop() { killall hibyshell hm_vmplayer hibynet 2>/dev/null; sleep 0.3; }
case "$1" in
stop) stop; exit 0 ;;
start|restart) ;;
*) echo "usage: shell_vm.sh start|stop|restart [nocache]"; exit 1 ;;
esac
stop
mkdir -p /dev/shm /usr/data /tmp/hibymod
grep -q " /dev/shm " /proc/mounts || mount -t tmpfs -o size=2m tmpfs /dev/shm
grep -q " /usr/data " /proc/mounts || mount -t tmpfs -o size=24m tmpfs /usr/data
mkdir -p $D/shell /usr/data/mnt/sd_0
grep -q " /usr/data/mnt/sd_0 " /proc/mounts || mount -t tmpfs -o size=48m tmpfs /usr/data/mnt/sd_0
cp -r $HS/shell/. $D/shell/
chmod +x $D/shell/hibynet
mkdir -p /usr/data/mnt/sd_0/Apps
cp $HS/sd/Apps/*.hpk /usr/data/mnt/sd_0/Apps/ 2>/dev/null
# language of the "player": int32 at offset 8 of user.ini (2 = English, 8 = Spanish, 10 = French)
L=$(cat $HS/lang 2>/dev/null || echo en)
case $L in es) LV=8 ;; fr) LV=10 ;; *) LV=2 ;; esac
printf "\\000\\000\\000\\000\\000\\000\\000\\000\\$(printf %03o $LV)\\000\\000\\000" > /usr/data/user.ini
URL=$(cat $HS/repo_url 2>/dev/null || echo http://10.0.2.2:8765/index.json)
[ "$URL" = none ] || echo "$URL" > $D/store_repos.conf
[ "$2" = nocache ] && rm -rf $D/cache/store
rm -f /tmp/vmplayer.log /tmp/hibymod/shell.log
$HS/bin/hm_vmplayer >/tmp/vmplayer.err 2>&1 &
sleep 0.5
$HS/bin/hibyshell >/tmp/hibyshell.err 2>&1 &
echo "hibyshell started (lang $L, repo $URL)"
