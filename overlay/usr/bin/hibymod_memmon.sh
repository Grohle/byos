#!/bin/sh
# hibymod: guarda en RAM la ultima foto de memoria y el minimo observado.
# Coste: una lectura de /proc/meminfo cada 20 s. Sirve para diagnosticar crashes.
D=/tmp/hibymod
mkdir -p $D
min=999999999
while true
do
    free=$(awk '/^MemFree:/{f=$2} /^Cached:/{c=$2} /^Buffers:/{b=$2} END{print f+c+b}' /proc/meminfo)
    if [ -n "$free" ] && [ "$free" -lt "$min" ]; then
        min=$free
        echo "min_free_kb=$min uptime=$(cut -d' ' -f1 /proc/uptime)" > $D/mem.min
    fi
    cat /proc/meminfo > $D/mem.last 2>/dev/null
    sleep 20
done
