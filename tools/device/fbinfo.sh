#!/bin/sh
cd /sys/class/graphics/fb0
for f in virtual_size bits_per_pixel stride modes pan name; do echo "$f: $(cat $f 2>/dev/null)"; done
grep -i -E 'fb|lcd|video|dpu' /proc/iomem
dmesg | grep -i -E 'fb|lcd|frame|smem' | head -20
p=$(pidof hiby_player)
grep -E 'fb|sa_hgl' /proc/$p/maps
