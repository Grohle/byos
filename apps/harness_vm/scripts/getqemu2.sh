#!/bin/bash
cd ~/hiby/vm/debs
sudo -n true 2>/dev/null && echo hasSudo
apt-get download -o Acquire::http::No-Cache=true libasound2-data libasound2t64 libnss3 2>&1 | tail -2
ls *.deb | grep -E "asound|nss3"
for f in libasound*.deb libnss3*.deb; do [ -f "$f" ] && dpkg -x "$f" ~/hiby/vm/qemu; done
Q=~/hiby/vm/qemu
export LD_LIBRARY_PATH=$Q/usr/lib/x86_64-linux-gnu:$Q/lib/x86_64-linux-gnu
ldd $Q/usr/bin/qemu-system-mipsel | grep "not found"
$Q/usr/bin/qemu-system-mipsel --version | head -1
$Q/usr/bin/qemu-system-mipsel -L $Q/usr/share/qemu -M malta -device help 2>&1 | grep -iE "vga|bochs|display|virtio-(gpu|blk|net)|pcnet|e1000" | head
