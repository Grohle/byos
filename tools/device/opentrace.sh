#!/bin/sh
# Ficheros que abre hiby_player al ejecutar una accion (toque en X,Y).
P=$(pidof hiby_player)
strace -f -e trace=open,openat -p $P -o /tmp/ot.txt 2>/dev/null &
S=$!; sleep 0.5
hibyctl tap ${1:-80} ${2:-105}
sleep ${3:-2}
kill $S; sleep 0.3
grep -v ENOENT /tmp/ot.txt | grep -oE '"[^"]+"' | sort | uniq -c | sort -rn | head -40
echo "--- no encontrados:"; grep ENOENT /tmp/ot.txt | grep -oE '"[^"]+"' | sort -u | head -10
