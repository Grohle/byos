#!/bin/sh
# Presentaciones del reproductor durante y despues de un deslizamiento: swipeframes.sh X1 Y1 X2 Y2 MS
fr() { od -An -tu4 -j16 -N4 /dev/shm/hibymod_fbstate | tr -d ' '; }
a=$(fr)
/tmp/hibyctl swipe $1 $2 $3 $4 $5 &
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24; do
    usleep 50000
    echo -n "$(( $(fr) - a )) "
done
echo
