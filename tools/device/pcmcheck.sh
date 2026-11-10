#!/bin/sh
p=$(pidof hiby_player)
echo "abierto ahora:"; ls -l /proc/$p/fd | grep snd
/tmp/hibyctl swipe 160 5 160 300 250; sleep 1
/tmp/hibyctl tap 160 397; sleep 3
echo "tras pausar/reanudar:"; ls -l /proc/$p/fd | grep snd
/tmp/hibyctl tap 160 397; sleep 3
echo "tras otra pulsacion:"; ls -l /proc/$p/fd | grep snd
/tmp/hibyctl swipe 160 470 160 150 250
