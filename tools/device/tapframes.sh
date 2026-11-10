#!/bin/sh
# Cuenta presentaciones (pans) del reproductor tras un toque: animaciones propias de HiBy
# uso: tapframes.sh X Y
fr() { od -An -tu4 -j16 -N4 /dev/shm/hibymod_fbstate | tr -d ' '; }
a=$(fr)
/tmp/hibyctl tap $1 $2 &
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    usleep 50000
    echo -n "$(( $(fr) - a )) "
done
echo
dd if=/dev/shm/hibymod_fbstate bs=1 skip=28 count=96 2>/dev/null | tr -d '\0'; echo
