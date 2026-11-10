#!/bin/sh
# hibyinput: inyecta toques y teclas en el HiBy R3 II (para pruebas por ADB).
#   hibyinput tap X Y              toque corto en (X,Y) pixeles de pantalla (320x480)
#   hibyinput hold X Y MS          pulsacion larga
#   hibyinput swipe X1 Y1 X2 Y2    deslizamiento (~250 ms)
#   hibyinput key CODE [DEV]       pulsa una tecla (por defecto event0; 116 = encendido)
# struct input_event en MIPS32: tv_sec(4) tv_usec(4) type(2) code(2) value(4), little endian.
TS=/dev/input/event1

b() { printf "\\$(printf %03o $(( $1 & 255 )))"; }
le16() { b $1; b $(( $1 >> 8 )); }
le32() { b $1; b $(( $1 >> 8 )); b $(( $1 >> 16 )); b $(( $1 >> 24 )); }
ev() { printf '\0\0\0\0\0\0\0\0'; le16 $1; le16 $2; le32 $3; }

down() { { ev 1 330 1; ev 3 53 $1; ev 3 54 $2; ev 3 48 22; ev 3 50 22; ev 3 57 0; ev 0 2 0; ev 0 0 0; } > $TS; }
move() { { ev 3 53 $1; ev 3 54 $2; ev 3 48 22; ev 3 50 22; ev 3 57 0; ev 0 2 0; ev 0 0 0; } > $TS; }
up()   { { ev 1 330 0; ev 0 2 0; ev 0 0 0; } > $TS; }

case "$1" in
  tap)   down $2 $3; sleep 0.06; move $2 $3; sleep 0.04; up ;;
  hold)  down $2 $3; n=$(( ${4:-800} / 50 )); i=0; while [ $i -lt $n ]; do sleep 0.05; move $2 $3; i=$((i+1)); done; up ;;
  swipe) x=$2; y=$3; down $x $y; i=1
         while [ $i -le 8 ]; do sleep 0.03; move $(( $2 + ($4 - $2) * i / 8 )) $(( $3 + ($5 - $3) * i / 8 )); i=$((i+1)); done
         up ;;
  key)   d=${3:-/dev/input/event0}; { ev 1 $2 1; ev 0 0 0; } > $d; sleep 0.08; { ev 1 $2 0; ev 0 0 0; } > $d ;;
  *)     sed -n '2,7p' "$0" ;;
esac
