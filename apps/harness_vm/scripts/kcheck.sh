#!/bin/bash
A=~/hiby/vm/apt/var/cache/apt/archives
K=~/hiby/vm/kernel
rm -rf $K; mkdir -p $K; dpkg -x $A/linux-image-6.1.0-50-4kc-malta*.deb $K
ls $K/boot
C=$(ls $K/boot/config-*)
grep -E "CONFIG_(FB_VIRTUAL|INPUT_UINPUT|INPUT_EVDEV|VIRTIO_BLK|VIRTIO_PCI|VIRTIO_NET|PCNET32|EXT4_FS|EXT2_FS|FRAMEBUFFER_CONSOLE|FB|ATA_PIIX|BLK_DEV_SD|SERIAL_8250|DEVTMPFS|DEVTMPFS_MOUNT|CPU_MIPS32_R2|HIGHMEM|PAGE_SIZE_4KB|ZRAM|MEMCG|DRM_BOCHS|FB_CIRRUS|DRM_CIRRUS_QEMU|E1000|UNIX|PACKET|VT|BLK_DEV_INITRD)=" $C
find $K/lib/modules -name "vfb.ko*" -o -name "uinput.ko*" -o -name "virtio_blk.ko*" -o -name "ext4.ko*" -o -name "pcnet32.ko*" -o -name "virtio_net.ko*" -o -name "virtio_pci*.ko*"
