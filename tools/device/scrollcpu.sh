#!/bin/sh
# CPU por hilo mientras se desplaza una lista 6 veces (toques inyectados).
P=$(pidof hiby_player)
for t in /proc/$P/task/*; do set -- $(cat $t/stat); echo "$1 $(( ${14} + ${15} ))"; done > /tmp/t0
u0=$(cut -d' ' -f1 /proc/uptime)
i=0; while [ $i -lt 6 ]; do /tmp/hibyctl swipe 160 420 160 80 200; sleep 0.4; i=$((i+1)); done
u1=$(cut -d' ' -f1 /proc/uptime)
echo "duracion: $(echo "$u0 $u1" | awk '{printf "%.1f s", $2-$1}')"
for t in /proc/$P/task/*; do
  set -- $(cat $t/stat); c=$(( ${14} + ${15} )); c0=$(awk -v p=$1 '$1==p{print $2}' /tmp/t0)
  echo "$(( c - ${c0:-0} )) $(cat $t/comm)"
done | sort -rn | head -6
echo "majflt: $(cut -d' ' -f12 /proc/$P/stat)"
