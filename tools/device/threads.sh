#!/bin/sh
# CPU por hilo de hiby_player durante N segundos (por defecto 10).
P=$(pidof hiby_player); S=${1:-10}
for t in /proc/$P/task/*; do set -- $(cat $t/stat); echo "$1 $(( ${14} + ${15} ))"; done > /tmp/t0
sleep $S
for t in /proc/$P/task/*; do
  set -- $(cat $t/stat); tid=$1; c=$(( ${14} + ${15} ))
  c0=$(awk -v p=$tid '$1==p{print $2}' /tmp/t0)
  echo "$(( c - ${c0:-0} )) $tid $(cat $t/comm) $(cat $t/wchan 2>/dev/null)"
done | sort -rn | head -12
echo "--- cambios de contexto del proceso"; grep -E 'ctxt' /proc/$P/status
