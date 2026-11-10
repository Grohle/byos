#!/bin/sh
# Musica -> deslizar atras -> cuanto tarda en verse el launcher (grabado)
/tmp/hibyctl tap 197 427; sleep 1.5
(/tmp/hibyctl rec 2500 /usr/data/hibymod/r5.bin 45 &)
sleep 0.05
t0=$(cut -d' ' -f1 /proc/uptime)
/tmp/hibyctl swipe 5 240 280 240 250
sleep 2.6
echo "deslizar en t=$t0"
tail -4 /tmp/hibymod/shell.log
