#!/bin/bash
# Arnes HiBy R3 II: qemu-system-mipsel (malta, MIPS32r2 LE) con fb 320x480x16 (vfb parcheado),
# 5 dispositivos evdev identicos via uinput (hibyinput), RAM limitada.
# Uso: run_vm.sh [--profile coexist|solo|none] [--icount] [--cpu 24KEc|24Kf] [--mem MB] [--interactive]
VM=$(cd "$(dirname "$0")" && pwd)
Q=$VM/qemu
export LD_LIBRARY_PATH=$Q/usr/lib/x86_64-linux-gnu:$Q/lib/x86_64-linux-gnu
PROFILE=none; ICOUNT=""; CPU=24KEc; MEM=${HIBY_MEM:-66248K}; INTER=0; PORT=${HIBY_PORT:-5555}
while [ $# -gt 0 ]; do case $1 in
  --profile) PROFILE=$2; shift;; --icount) ICOUNT="-icount shift=0,sleep=on";; --cpu) CPU=$2; shift;;
  --mem) MEM=$2; shift;; --interactive) INTER=1;; --port) PORT=$2; shift;; esac; shift; done
mkdir -p $VM/share $VM/logs
if [ $INTER = 1 ]; then SER="-serial mon:stdio"; else SER="-serial file:$VM/logs/serial.log"; fi
exec $Q/usr/bin/qemu-system-mipsel -L $Q/usr/share/qemu -M malta -cpu $CPU -m 128 $ICOUNT \
  -kernel $VM/out/vmlinux-hiby \
  -append "root=/dev/vda rw rootwait console=ttyS0 init=/sbin/hiby-init mem=${MEM} video=vfb: hiby.mem=$PROFILE noirqdebug quiet" \
  -drive file=$VM/rootfs.ext4,if=none,id=d0,format=raw -device virtio-blk-pci,drive=d0 \
  -netdev user,id=n0,hostfwd=tcp:127.0.0.1:$PORT-:5555 -device virtio-net-pci,netdev=n0,romfile= \
  -fsdev local,id=fs0,path=$VM/share,security_model=none -device virtio-9p-pci,fsdev=fs0,mount_tag=share \
  -vga none -display none -nographic -monitor none $SER
