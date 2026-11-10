#!/bin/sh
# Abre Musica desde el launcher, vuelve con "atras" y mide cuando reaparece el launcher.
/tmp/hibyctl tap 115 427; sleep 1.5
t0=$(cut -d' ' -f1 /proc/uptime)
/tmp/hibyctl tap 22 48
sleep 3
echo "atras en t=$t0 (s de uptime)"
tail -5 /tmp/hibymod/shell.log
