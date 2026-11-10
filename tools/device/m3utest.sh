#!/bin/sh
# Prueba: lista .m3u en la SD con rutas relativas (para las mezclas a peticion)
D="/usr/data/mnt/sd_0/0 Mezclas"
mkdir -p "$D"
printf '#EXTM3U\n../Guitarricadelafuente/Abc/1-01 Abc.flac\n../Carolina Durante/Cuatro Chavales/1-07 Aaaaaa#$!&.flac\n' > "$D/Prueba relativa.m3u"
printf '#EXTM3U\n/usr/data/mnt/sd_0/Guitarricadelafuente/Abc/1-01 Abc.flac\n/usr/data/mnt/sd_0/Carolina Durante/Cuatro Chavales/1-07 Aaaaaa#$!&.flac\n' > "$D/Prueba absoluta.m3u"
ls -la "$D"
ls "/usr/data/mnt/sd_0/Guitarricadelafuente/Abc/" | head -3
sync
