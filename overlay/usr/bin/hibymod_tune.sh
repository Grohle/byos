#!/bin/sh
# hibymod: ajustes de kernel en tiempo de ejecucion (sin swap ni zram en este kernel).
# Cada ajuste se aplica solo si el fichero existe; nunca aborta el arranque.

w() { [ -w "$1" ] && echo "$2" > "$1" 2>/dev/null; }

# Liberar antes la cache de dentries/inodes: con bibliotecas enormes
# (decenas de miles de ficheros) esta cache se come la poca RAM disponible.
w /proc/sys/vm/vfs_cache_pressure 200

# Reserva minima para asignaciones atomicas (SD, audio, wifi). Con muy poca
# reserva, una asignacion fallida en un driver puede tumbar el sistema.
mem_kb=$(awk '/^MemTotal:/{print $2}' /proc/meminfo)
cur=$(cat /proc/sys/vm/min_free_kbytes 2>/dev/null)
if [ -n "$mem_kb" ] && [ -n "$cur" ]; then
    want=$((mem_kb / 32))
    [ $want -gt 4096 ] && want=4096
    [ $want -gt $cur ] && w /proc/sys/vm/min_free_kbytes $want
fi

# Escritura diferida acotada: poca memoria sucia y vaciado rapido (5 s).
# Compensa haber quitado "-o sync" del montaje de /usr/data.
w /proc/sys/vm/dirty_background_ratio 5
w /proc/sys/vm/dirty_ratio 10
w /proc/sys/vm/dirty_expire_centisecs 500
w /proc/sys/vm/dirty_writeback_centisecs 300

# Sobrecompromiso siempre permitido. En modo heuristico (0) este kernel rechaza un fork() cuando
# una sola zona privada del proceso (las pilas de 8 MB de los hilos de adbd o de la interfaz) no
# cabe en la memoria libre + cache: con menos de ~8 MB libres "adb shell" responde "error: closed"
# y las descargas de las apps no arrancan, aunque el hijo apenas vaya a usar memoria.
w /proc/sys/vm/overcommit_memory 1

exit 0
