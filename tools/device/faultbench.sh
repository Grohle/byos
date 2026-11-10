#!/bin/sh
# Coste de leer paginas "en frio" (como un fallo de pagina) en squashfs vs UBIFS.
# Lee 40 paginas de 4 KB en posiciones repartidas del binario, tras vaciar la cache.
SRC=/usr/bin/hiby_player
cp $SRC /usr/data/hp_bench.bin
SIZE=$(wc -c < $SRC); PAGES=$((SIZE / 4096))
bench()
{
    f=$1
    sync; echo 3 > /proc/sys/vm/drop_caches
    t0=$(cut -d' ' -f1 /proc/uptime)
    i=0
    while [ $i -lt 40 ]; do
        dd if=$f of=/dev/null bs=4096 skip=$(( (i * 7919) % PAGES )) count=1 2>/dev/null
        i=$((i + 1))
    done
    t1=$(cut -d' ' -f1 /proc/uptime)
    echo "$2: 40 lecturas aleatorias en frio = $(echo "$t0 $t1" | awk '{printf "%.0f ms (%.1f ms/pagina)", ($2-$1)*1000, ($2-$1)*1000/40}')"
}
bench $SRC "squashfs (sistema)"
bench /usr/data/hp_bench.bin "ubifs (/usr/data)"
# referencia: la misma operacion con todo en cache
t0=$(cut -d' ' -f1 /proc/uptime); i=0
while [ $i -lt 40 ]; do dd if=$SRC of=/dev/null bs=4096 skip=$(( (i * 7919) % PAGES )) count=1 2>/dev/null; i=$((i+1)); done
t1=$(cut -d' ' -f1 /proc/uptime)
echo "en cache (referencia): $(echo "$t0 $t1" | awk '{printf "%.0f ms", ($2-$1)*1000}')"
rm -f /usr/data/hp_bench.bin; sync
