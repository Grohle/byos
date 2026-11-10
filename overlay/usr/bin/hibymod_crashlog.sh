#!/bin/sh
# hibymod: informe de cierre/crash de hiby_player.
# Uso: hibymod_crashlog.sh <crash|exit0|boot> <rc> <t_inicio> <t_fin>
kind=$1; rc=$2; t0=$3; t1=$4
D=/tmp/hibymod
SD=/data/mnt/sd_0
mkdir -p $D
f=$D/last_$kind.txt
{
    echo "=== hibymod $kind  rc=$rc  start_uptime=$t0  end_uptime=$t1"
    if [ -n "$rc" ] && [ "$rc" -gt 128 ] 2>/dev/null && [ "$rc" -lt 160 ]; then
        echo "terminado por la senal $((rc - 128)) (11=SIGSEGV 6=SIGABRT 9=SIGKILL/OOM 7=SIGBUS 15=SIGTERM)"
    else
        echo "codigo de salida $rc"
    fi
    echo "--- date"; date
    echo "--- /proc/jz/reset/reset"; cat /proc/jz/reset/reset 2>/dev/null
    echo "--- meminfo now"; cat /proc/meminfo
    echo "--- memmon"; cat $D/mem.min 2>/dev/null
    echo "--- df"; df 2>/dev/null
    echo "--- mounts"; cat /proc/mounts
    echo "--- /data"; ls -la /data/ 2>/dev/null
    echo "--- ps"; ps 2>/dev/null
    echo "--- dmesg (tail)"; dmesg 2>/dev/null | tail -150
} > $f 2>&1

# Copiar a la SD si esta montada (no falla si no lo esta)
if grep -qE " (/usr)?/data/mnt/sd_0 " /proc/mounts 2>/dev/null; then
    mkdir -p $SD/hibymod/logs 2>/dev/null
    n=$(date +%Y%m%d_%H%M%S 2>/dev/null)
    cp $f "$SD/hibymod/logs/${kind}_${n}.txt" 2>/dev/null
    # mantener como mucho 30 informes
    ls -1t $SD/hibymod/logs/*.txt 2>/dev/null | sed -n '31,$p' | while read old; do rm -f "$old"; done
    sync
fi
exit 0
