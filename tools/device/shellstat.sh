#!/bin/sh
# Estado de hibyshell: memoria, CPU acumulada y ultimas lineas del registro
P=$(pidof hibyshell)
echo "pid $P"
grep -E 'VmRSS|VmHWM' /proc/$P/status
echo "cpu (utime stime en ticks): $(cut -d' ' -f14,15 /proc/$P/stat)"
grep MemAvailable /proc/meminfo
ls -d /usr/data/mnt/sd_0/ABBA/* 2>/dev/null
tail -4 /tmp/hibymod/shell.log
