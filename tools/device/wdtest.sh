#!/bin/sh
# Prueba de la vigilancia: congela hibyshell y comprueba que la libreria lo mata,
# que la tactil vuelve al reproductor y que el supervisor lo relanza.
P=$(pidof hibyshell)
echo "shell $P, entrada $(od -An -tu4 -j20 -N4 /dev/shm/hibymod_fbstate)"
kill -STOP $P
sleep 6
echo "tras congelarlo: shell '$(pidof hibyshell)', entrada $(od -An -tu4 -j20 -N4 /dev/shm/hibymod_fbstate)"
sleep 6
echo "relanzado: $(pidof hibyshell), entrada $(od -An -tu4 -j20 -N4 /dev/shm/hibymod_fbstate)"
cat /tmp/hibymod/shell_exits.txt
