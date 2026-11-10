#!/bin/sh
# Fotogramas por segundo mientras se desplaza la lista actual (3 deslizamientos).
/tmp/hibyctl fps 3500 &
sleep 0.2
for i in 1 2 3; do /tmp/hibyctl swipe 160 420 160 80 250; sleep 0.6; done
wait
