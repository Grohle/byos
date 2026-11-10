#!/bin/bash
# Compila un kernel Linux 6.1 LE MIPS32r2 para qemu malta con: vfb parcheado (320x480x16, virtual 320x960),
# uinput/evdev, virtio blk/net/9p, ext4, sin modulos, sin fbcon. Sin root.
set -e
X=~/hiby/vm/xtc
export PATH=$X/usr/bin:$PATH
export BISON_PKGDATADIR=$X/usr/share/bison M4=$X/usr/bin/m4
export LD_LIBRARY_PATH=$X/usr/lib/x86_64-linux-gnu:$X/lib/x86_64-linux-gnu
ln -sf mipsel-linux-gnu-gcc-13 $X/usr/bin/mipsel-linux-gnu-gcc
ln -sf mipsel-linux-gnu-cpp-13 $X/usr/bin/mipsel-linux-gnu-cpp
cd ~/hiby/vm/src/linux-6.1.192
python3 ~/hiby/vm/scripts/patch_vfb.py drivers/video/fbdev/vfb.c
M="make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu-"
$M malta_defconfig
S=scripts/config
$S -d CPU_BIG_ENDIAN -e CPU_LITTLE_ENDIAN -e CPU_MIPS32_R2 -d CPU_MIPS32_R6 -d SMP -d MIPS_MT_SMP
$S -e MODULES -d MODULE_SIG -d HIGHMEM -e DEVTMPFS -e DEVTMPFS_MOUNT -e TMPFS -e BLK_DEV_INITRD
$S -e PCI -e VIRTIO_MENU -e VIRTIO -e VIRTIO_PCI -e VIRTIO_PCI_LEGACY -e VIRTIO_BLK -e VIRTIO_NET -e NETDEVICES -e NET_CORE
$S -e NET -e INET -e UNIX -e PACKET -d IPV6
$S -e NET_9P -e NET_9P_VIRTIO -e 9P_FS -e EXT4_FS -e EXT2_FS
$S -e FB -e FB_VIRTUAL -d FB_CIRRUS -d FB_MATROX -d FRAMEBUFFER_CONSOLE -d LOGO -d DRM
$S -e INPUT -e INPUT_EVDEV -e INPUT_MISC -e INPUT_UINPUT -d INPUT_KEYBOARD -d INPUT_MOUSE -d SERIO -d SERIO_I8042 -d INPUT_MOUSEDEV
$S -e VT -e VT_CONSOLE -e UNIX98_PTYS
$S -e SERIAL_8250 -e SERIAL_8250_CONSOLE
$S -d SOUND -d USB_SUPPORT -d WLAN -d WIRELESS -d CFG80211 -d MD -d SCSI -d ATA -d NFS_FS -d NETFILTER -d BRIDGE -d BT -d HAMRADIO -d CAN
$S -e PROC_FS -e SYSFS -e SWAP -e PRINTK -e MEMCG -e CGROUPS
$S -e HZ_250 -d SYSTEM_TRUSTED_KEYRING -d SYSTEM_DATA_VERIFICATION -d MODULE_SIG -d INTEGRITY -d IMA -d SECONDARY_TRUSTED_KEYRING -d SYSTEM_BLACKLIST_KEYRING -d PKCS7_MESSAGE_PARSER -d MODULE_SIG_FORMAT
$M olddefconfig
grep -E "CONFIG_(CPU_LITTLE_ENDIAN|FB_VIRTUAL|INPUT_UINPUT|VIRTIO_BLK|9P_FS|EXT4_FS|MODULES|FRAMEBUFFER_CONSOLE|SMP)[= ]" .config || true
$M -j20 vmlinux 2>&1 | grep -E "error|Error|warning: " | head -30
ls -la vmlinux
mipsel-linux-gnu-strip -o ~/hiby/vm/out/vmlinux-hiby vmlinux
cp .config ~/hiby/vm/out/kernel.config
ls -la ~/hiby/vm/out/vmlinux-hiby
grep -c "=y" .config; grep -c "=m" .config
