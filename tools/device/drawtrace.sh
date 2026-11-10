#!/bin/sh
# Como presenta hiby_player cada fotograma: mapeos de video e ioctls durante una accion.
P=$(pidof hiby_player)
echo "--- mapeos de video"
grep -E 'fb0|sa_hgl|sa-config' /proc/$P/maps
echo "--- descriptores"
ls -la /proc/$P/fd 2>/dev/null | grep -E 'fb0|sa_hgl|sa-config' | awk '{print $(NF-2), $NF}'
strace -f -tt -e trace=ioctl,mmap2,munmap,write -p $P -o /tmp/dt.txt 2>/dev/null &
S=$!; sleep 0.5
hibyctl tap ${1:-80} ${2:-105}
sleep 1.5
kill $S; sleep 0.3
echo "--- ioctls por fd y codigo"
grep ioctl /tmp/dt.txt | grep -oE 'ioctl\([0-9]+, [A-Z_0-9x]+' | sort | uniq -c | sort -rn | head -20
echo "--- ejemplos"
grep ioctl /tmp/dt.txt | grep -vE 'TCGETS|SNDRV|FIONREAD' | head -12
