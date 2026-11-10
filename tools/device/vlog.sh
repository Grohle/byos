#!/bin/sh
# Muestra el registro de vistas abiertas (hibymod_disp v4): secuencia y ultimas 16
S=/dev/shm/hibymod_fbstate
seq=$(od -An -tu4 -j36 -N4 $S | tr -d ' ')
echo "seq=$seq"
i=0
while [ $i -lt 16 ]; do
    n=$(( (seq - 16 + i) ))
    if [ $n -ge 0 ]; then
        slot=$(( n % 16 ))
        echo "$n: $(dd if=$S bs=1 skip=$((40 + slot * 40)) count=40 2>/dev/null | tr -d '\0')"
    fi
    i=$((i + 1))
done
