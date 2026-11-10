#!/bin/sh
# Desplazamiento arriba/abajo sobre las MISMAS filas (contenido ya preparado).
hibyctl swipe 160 420 160 80 250; sleep 0.6; hibyctl swipe 160 80 160 420 250; sleep 0.8
FPS_TIMES=1 /tmp/hibyctl fps 8000 &
F=$!; sleep 0.2
i=0; while [ $i -lt 5 ]; do
  hibyctl swipe 160 420 160 80 250; sleep 0.55
  hibyctl swipe 160 80 160 420 250; sleep 0.55
  i=$((i+1))
done
wait $F
