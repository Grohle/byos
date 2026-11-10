#!/bin/sh
# Correlacion entre tirones de pantalla y procesos lanzados por hiby_player.
P=$(pidof hiby_player)
strace -f -tt -e trace=execve,clone -p $P -o /tmp/hc.txt 2>/dev/null &
S=$!
sleep 0.5
FPS_TIMES=1 /tmp/hibyctl fps 9000 > /tmp/fps.txt &
F=$!
sleep 0.2
i=0; while [ $i -lt 10 ]; do /tmp/hibyctl swipe 160 420 160 80 250; sleep 0.55; i=$((i+1)); done
wait $F
kill $S 2>/dev/null; sleep 0.3
cat /tmp/fps.txt
echo "--- procesos creados (hora de clone)"
grep -E 'clone\(' /tmp/hc.txt | grep -v 'CLONE_THREAD' | awk '{print "  clone a las", $2}'
echo "--- hilos creados"
grep -c 'CLONE_THREAD' /tmp/hc.txt
