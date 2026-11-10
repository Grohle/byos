#!/bin/sh
# Prueba en vivo (hasta reiniciar): activa todas las opciones ocultas de Ajustes y reinicia la interfaz.
src=/usr/resource/set_functions.json
cp $src /tmp/set_functions.json
sed -i 's/{"\([a-z_]*\)":0}/{"\1":1}/g' /tmp/set_functions.json
grep -c '":1}' /tmp/set_functions.json
grep -q " $src " /proc/mounts || mount -o bind /tmp/set_functions.json $src
diff /tmp/set_functions.json $src >/dev/null && echo "montado"
# reinicio de la interfaz: el supervisor lo relanza en ~2 s
killall hiby_player
