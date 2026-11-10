#!/bin/sh
# Busca imagenes en la SD (sin bajar a carpetas de musica profundas) y prueba hibywall
SD=/usr/data/mnt/sd_0
find $SD -maxdepth 3 \( -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.png' \) 2>/dev/null | head -8 > /tmp/imgs.txt
cat /tmp/imgs.txt
f=$(head -1 /tmp/imgs.txt)
[ -n "$f" ] || exit 0
ls -la "$f"
/usr/bin/time -v /usr/data/hibymod/shell/hibywall "$f" /tmp/w.raw 2>&1 | grep -E 'x|Maximum resident|Elapsed' | head -5 \
  || { s=$(cut -d' ' -f1 /proc/uptime); /usr/data/hibymod/shell/hibywall "$f" /tmp/w.raw; echo "rc=$? de $s a $(cut -d' ' -f1 /proc/uptime)"; }
