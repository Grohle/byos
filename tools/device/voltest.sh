#!/bin/sh
# Sube 3 pasos y baja 3 con la rueda; tras cada paso guarda la barra de estado (30 filas)
# para comparar la cifra real con la que lee hibyshell (shell.log).
rm -f /usr/data/hibymod/top_*.raw /tmp/top_*
i=0
for k in 106 106 106 105 105 105; do
    /tmp/hibyctl key $k event3
    sleep 0.7
    /tmp/hibyctl shot /tmp/s.raw 2>/dev/null
    dd if=/tmp/s.raw of=/usr/data/hibymod/top_$i.raw bs=640 count=30 2>/dev/null
    i=$((i + 1))
done
rm -f /tmp/s.raw
grep -E 'volumen' /tmp/hibymod/shell.log | tail -14
