#!/bin/sh
# Diagnostico de interfaz congelada: estado de cada hilo de hiby_player (solo lectura).
P=$(pidof hiby_player)
echo "pid=$P  uptime=$(cut -d' ' -f1 /proc/uptime)"
echo "TID  ESTADO  CPU(utime+stime)  WCHAN  NOMBRE"
for t in /proc/$P/task/*; do
  tid=$(basename $t)
  st=$(cut -d' ' -f3 $t/stat)
  ut=$(cut -d' ' -f14 $t/stat); stt=$(cut -d' ' -f15 $t/stat)
  wc=$(cat $t/wchan 2>/dev/null)
  echo "$tid $st $((ut+stt)) $wc $(cat $t/comm)"
done
sleep 3
echo "--- CPU 3 s despues"
for t in /proc/$P/task/*; do
  echo "$(basename $t) $(( $(cut -d' ' -f14 $t/stat) + $(cut -d' ' -f15 $t/stat) )) $(cat $t/comm)"
done
echo "--- fds de dispositivos"
ls -la /proc/$P/fd 2>/dev/null | grep -E '/dev/' | awk '{print $NF}' | sort | uniq -c
echo "--- audio"; cat /proc/asound/card*/pcm*p/sub0/status 2>/dev/null | head -8
echo "--- dmesg reciente"; dmesg | tail -15
