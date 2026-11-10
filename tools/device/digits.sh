#!/bin/sh
# Captura la barra de estado con el volumen en N..N+K para sacar plantillas de digitos
# reales de HiBy; despues deja el volumen como estaba.
UP=$1
rm -f /usr/data/hibymod/dg_*.raw
i=0
/tmp/hibyctl shot /tmp/s.raw 2>/dev/null; dd if=/tmp/s.raw of=/usr/data/hibymod/dg_$i.raw bs=640 count=30 2>/dev/null
while [ $i -lt $UP ]; do
    i=$((i + 1))
    /tmp/hibyctl key 106 event3; sleep 0.6
    /tmp/hibyctl shot /tmp/s.raw 2>/dev/null; dd if=/tmp/s.raw of=/usr/data/hibymod/dg_$i.raw bs=640 count=30 2>/dev/null
done
while [ $i -gt 0 ]; do /tmp/hibyctl key 105 event3; sleep 0.4; i=$((i - 1)); done
rm -f /tmp/s.raw
ls /usr/data/hibymod/dg_*.raw | wc -l
