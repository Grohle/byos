#!/bin/sh
# Instantanea de rendimiento de hiby_player: fallos de pagina mayores (lecturas de flash),
# CPU y memoria. Se ejecuta antes y despues de navegar para ver la diferencia.
P=$(pidof hiby_player)
set -- $(cat /proc/$P/stat)
# campos: 10 minflt, 12 majflt, 14 utime, 15 stime
echo "majflt=${12} minflt=${10} cpu=$(( ${14} + ${15} )) $(grep -E 'MemFree|^Cached|MemAvailable' /proc/meminfo | tr -s ' ' | tr '\n' ' ') rss=$(awk '/VmRSS/{print $2}' /proc/$P/status) pgmajfault_total=$(awk '/pgmajfault/{print $2}' /proc/vmstat)"
