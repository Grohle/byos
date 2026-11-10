#!/bin/sh
# Memoria propia aproximada (residente - compartida, de /proc/PID/statm) y CPU en reposo.
snap() { for p in /proc/[0-9]*; do [ -f $p/stat ] || continue; set -- $(cat $p/stat 2>/dev/null); echo "$1 $(( ${14} + ${15} ))"; done; }
snap > /tmp/c0; sleep 20; snap > /tmp/c1
echo "PID  CPU20s  RSS_kB  PROPIA_kB  NOMBRE"
for p in /proc/[0-9]*; do
  pid=$(basename $p)
  set -- $(cat $p/statm 2>/dev/null); [ -n "$2" ] || continue
  [ "$2" -gt 0 ] || continue
  rss=$(( $2 * 4 )); own=$(( ($2 - $3) * 4 ))
  c0=$(awk -v p=$pid '$1==p{print $2}' /tmp/c0); c1=$(awk -v p=$pid '$1==p{print $2}' /tmp/c1)
  echo "$pid $(( ${c1:-0} - ${c0:-0} )) $rss $own $(cat $p/comm)"
done | sort -k4 -rn
echo "--- totales"; grep -E 'MemFree|MemAvailable|^Cached|AnonPages|Slab|SUnreclaim' /proc/meminfo | tr -s ' ' | tr '\n' ' '; echo
