#!/bin/sh
# Registra que vistas abre el reproductor en cada paso de navegacion.
S=/dev/shm/hibymod_fbstate
C=/tmp/hibyctl
seqv() { od -An -tu4 -j36 -N4 $S | tr -d ' '; }
step() {
    a=$(seqv)
    shift 0
    "$@" >/dev/null 2>&1
    sleep 1.3
    b=$(seqv)
    out=""
    n=$a
    while [ $n -lt $b ]; do
        out="$out $(dd if=$S bs=1 skip=$((40 + (n % 16) * 40)) count=40 2>/dev/null | tr -d '\0')"
        n=$((n + 1))
    done
    echo "[$*] ->$out"
}
step $C tap 22 48
step $C tap 22 48
step $C tap 116 425
step $C tap 80 150
step $C swipe 300 240 40 240 120
step $C tap 22 48
step $C tap 22 48
step $C tap 22 48
step $C swipe 300 240 40 240 120
step $C tap 22 48
step $C tap 202 425
step $C swipe 160 5 160 300 250
step $C swipe 160 470 160 150 250
step $C tap 22 48
step $C swipe 160 5 160 300 250
step $C swipe 160 470 160 150 250
