#!/bin/sh
# Diagnostico completo del aparato (solo lectura).
echo "=== procesos por RSS (kB)"
for p in /proc/[0-9]*; do
  n=$(cat $p/comm 2>/dev/null)
  r=$(awk '/VmRSS/{print $2}' $p/status 2>/dev/null)
  [ -n "$r" ] && echo "$r $n $(basename $p)"
done | sort -rn | head -22
echo "=== meminfo"; head -22 /proc/meminfo
echo "=== slab"; grep -E 'Slab|SReclaimable|SUnreclaim' /proc/meminfo
echo "=== mtd"; cat /proc/mtd
echo "=== mounts"; cat /proc/mounts
echo "=== df"; df
echo "=== slot"; hibymod slot
echo "=== sysctl"
for f in vfs_cache_pressure min_free_kbytes dirty_ratio dirty_background_ratio dirty_expire_centisecs; do echo "$f=$(cat /proc/sys/vm/$f)"; done
echo "oom hiby_player=$(cat /proc/$(pidof hiby_player)/oom_score_adj 2>/dev/null)"
echo "=== cmdline"; cat /proc/cmdline
echo "=== cpu"; head -14 /proc/cpuinfo; ls /sys/devices/system/cpu/cpu0/cpufreq 2>&1 | head -3
echo "=== fb"; cat /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null
echo "=== input"; grep -E 'Name|Handlers' /proc/bus/input/devices
echo "=== /data"; ls -la /data/
echo "=== db sizes"; ls -la /data/*.db /data/mnt/sd_0/.temp/ 2>/dev/null
echo "=== hiby_player status"; grep -E 'VmPeak|VmSize|VmHWM|VmRSS|Threads' /proc/$(pidof hiby_player)/status
echo "=== uptime/load"; cat /proc/uptime /proc/loadavg
echo "=== theme setting (mtd5 0x20000)"; nanddump -q -s 0x20000 -l 7 /dev/mtd5 -a 2>/dev/null; echo
echo "=== dmesg errors"; dmesg | grep -iE 'error|fail|oom|exfat|killed' | tail -20
