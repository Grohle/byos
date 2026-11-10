#!/bin/bash
# Construye rootfs.ext4 para el guest: Debian 12 mipsel (netsurf-fb, links2, w3m, busybox-static) + herramientas hiby.
set -e
VM=~/hiby/vm; R=$VM/rootfs; A=$VM/apt/var/cache/apt/archives
if [ ! -f $R/.extracted ]; then
  rm -rf $R; mkdir -p $R
  for f in $A/*.deb; do case $f in *linux-image*) continue;; esac; dpkg -x $f $R; done
  touch $R/.extracted
fi
cd $R
mkdir -p proc sys dev tmp run root mnt/share etc/ssl/certs hiby/bin var/tmp
# busybox applets
QU=$VM/qemu/usr/bin/qemu-mipsel-static
for a in $($QU -L $R $R/bin/busybox --list); do
  [ -e bin/$a ] || [ -e usr/bin/$a ] || [ -e sbin/$a ] || [ -e usr/sbin/$a ] || ln -s /bin/busybox bin/$a
done
cp /etc/ssl/certs/ca-certificates.crt etc/ssl/certs/ca-certificates.crt
echo "nameserver 10.0.2.3" > etc/resolv.conf
echo "root:x:0:0:root:/root:/bin/sh" > etc/passwd
echo "root:x:0:" > etc/group
echo hiby-vm > etc/hostname
cp $VM/out/bin/* hiby/bin/
cp $VM/guest/* hiby/ 2>/dev/null || true
chmod +x hiby/*.sh hiby/bin/* 2>/dev/null || true
ln -sf /hiby/init.sh sbin/hiby-init
cat > etc/hibyinput.conf <<'EOC'
# Codigos de tecla por dispositivo (event0..4). event1 es el touch y se ignora aqui.
# Ajustar cuando se conozcan los codigos reales del HiBy.
keys 0 116
keys 2 115 114 164 163 165
keys 3 115 114 164 163 165
keys 4 164 115 114
route POWER 0
route VOLUMEUP 3
route VOLUMEDOWN 3
route PLAYPAUSE 3
route NEXTSONG 3
route PREVIOUSSONG 3
release_mt_report 1
EOC
du -sh $R
rm -f $VM/rootfs.ext4
mke2fs -q -t ext4 -L hibyroot -E root_owner=0:0 -d $R $VM/rootfs.ext4 1200M
ls -la $VM/rootfs.ext4
