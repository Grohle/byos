#!/bin/sh
# Todos los fotogramas de un unico deslizamiento, con la hora de inicio y fin del gesto.
FPS_ALL=1 /tmp/hibyctl fps 2500 > /tmp/one.txt &
F=$!; sleep 0.3
echo "gesto empieza: $(date +%M:%S).$(cut -d' ' -f1 /proc/uptime)"
/tmp/hibyctl swipe 160 420 160 80 250
echo "gesto termina: $(date +%M:%S) uptime $(cut -d' ' -f1 /proc/uptime)"
wait $F
cat /tmp/one.txt
