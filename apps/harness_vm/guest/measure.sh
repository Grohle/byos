#!/bin/sh
# measure.sh SEGUNDOS CMD... : lanza CMD, muestrea VmRSS cada 0.5 s, al final imprime pico (VmHWM),
# RSS final, MemAvailable min, y si lo mato el OOM killer.
dur=$1; shift
a0=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
t0=$(cut -d' ' -f1 /proc/uptime)
"$@" >/tmp/measure.out 2>&1 &
pid=$!
minav=$a0; i=0; first=""
while [ $i -lt $((dur*2)) ]; do
  sleep 0.5; i=$((i+1))
  [ -d /proc/$pid ] || break
  av=$(awk '/MemAvailable/{print $2}' /proc/meminfo); [ $av -lt $minav ] && minav=$av
  rss=$(awk '/VmRSS/{print $2}' /proc/$pid/status 2>/dev/null)
  echo "t=$(awk -v a=$t0 '{printf "%.1f", $1-a}' /proc/uptime)s rss=${rss}kB avail=${av}kB"
done
if [ -d /proc/$pid ]; then
  grep -E "VmPeak|VmHWM|VmRSS|RssAnon|RssFile|VmData" /proc/$pid/status
  kill $pid; sleep 1; kill -9 $pid 2>/dev/null
  echo "RESULT alive=yes"
else
  wait $pid; echo "RESULT alive=no exit=$?"
fi
echo "MemAvailable inicio=${a0}kB min=${minav}kB"
dmesg | grep -iE "out of memory|oom-kill|Killed process" | tail -3
echo "--- salida (ultimas 15 lineas) ---"; tail -15 /tmp/measure.out
