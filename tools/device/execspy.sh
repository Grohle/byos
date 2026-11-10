#!/bin/sh
# Que procesos lanza hiby_player (system/popen) durante N segundos en reposo.
P=$(pidof hiby_player); S=${1:-20}
strace -f -tt -e trace=execve,vfork,clone -p $P -o /tmp/ex.txt &
X=$!; sleep $S; kill $X; sleep 0.5
echo "procesos creados en $S s: $(grep -c 'clone(\|vfork(' /tmp/ex.txt) (incluye hilos)"
echo "--- comandos ejecutados"
grep execve /tmp/ex.txt | grep -v ENOENT | sed -E 's/^[0-9]+ +//' | cut -c1-200 | head -40
