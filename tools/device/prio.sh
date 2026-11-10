#!/bin/sh
# Politica y prioridad de cada hilo de hiby_player (stat: 18 prio, 19 nice, 40 rt_priority, 41 policy).
P=$(pidof hiby_player)
echo "TID POLITICA RTPRIO NICE NOMBRE   (politica: 0=normal 1=FIFO 2=RR)"
for t in /proc/$P/task/*; do
  s=$(cat $t/stat); name=$(cat $t/comm)
  set -- $(echo "$s" | sed 's/.*) //')   # campos desde el 3
  # tras quitar "pid (comm) ", $1 es el campo 3 -> campo N = $((N-2))
  eval nice=\${17}; eval rtp=\${38}; eval pol=\${39}
  echo "$(basename $t) $pol $rtp $nice $name"
done
