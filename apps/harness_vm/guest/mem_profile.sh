#!/bin/sh
# Ajusta la RAM libre del guest a los presupuestos medidos en el HiBy:
#   coexist: reproductor de serie vivo (~15 MB) -> deja ~10 MB disponibles para la app
#   solo:    reproductor parado -> deja ~38 MB disponibles
# Calcula cuanto "memhog" hace falta para que MemAvailable quede en el objetivo.
case "$1" in coexist) target=10240;; solo) target=38912;; *) target=${1:-10240};; esac
avail=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
hog=$(( (avail - target) / 1024 ))
if [ $hog -gt 0 ]; then /hiby/bin/memhog $hog & sleep 2; fi
echo "mem_profile $1: objetivo ${target} kB; ahora: $(grep MemAvailable /proc/meminfo)"
