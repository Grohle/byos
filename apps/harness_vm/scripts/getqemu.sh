#!/bin/bash
set -e
cd ~/hiby/vm/debs
pkgs=$(apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks --no-replaces --no-enhances qemu-system-mips qemu-user-static qemu-utils 2>/dev/null | grep "^[a-z]" | sort -u)
need=""
for p in $pkgs; do
  if ! dpkg -s "$p" >/dev/null 2>&1; then need="$need $p"; fi
done
echo "NEED: $need"
apt-get download $need 2>&1 | tail -3
for f in *.deb; do dpkg -x "$f" ~/hiby/vm/qemu; done
ls ~/hiby/vm/qemu/usr/bin
