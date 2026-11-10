#!/bin/sh
# PID 1 del guest del arnes HiBy.
export PATH=/hiby/bin:/usr/sbin:/usr/bin:/sbin:/bin
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev 2>/dev/null
mkdir -p /dev/pts && mount -t devpts devpts /dev/pts
mount -t tmpfs -o size=4m tmpfs /tmp
mount -t 9p -o trans=virtio,version=9p2000.L share /mnt/share 2>/dev/null && echo "9p: /mnt/share montado"
[ -x /sbin/ldconfig ] && /sbin/ldconfig 2>/dev/null
hostname hiby-vm
ip link set lo up
ip link set eth0 up && ip addr add 10.0.2.15/24 dev eth0 && ip route add default via 10.0.2.2
# Dispositivos de entrada identicos al HiBy (event0..event4)
/hiby/bin/hibyinput 5555 &
sleep 1
for e in /sys/class/input/event*; do echo "$(basename $e): $(cat $e/device/name)"; done
grep -E "MemTotal|MemFree|MemAvailable" /proc/meminfo
# Perfil de memoria (hiby.mem=coexist|solo en la cmdline del kernel)
case "$(cat /proc/cmdline)" in
  *hiby.mem=coexist*) /hiby/mem_profile.sh coexist ;;
  *hiby.mem=solo*)    /hiby/mem_profile.sh solo ;;
esac
[ -x /mnt/share/autorun.sh ] && /mnt/share/autorun.sh
echo "HIBY-VM READY"
while true; do setsid sh -c 'exec sh </dev/ttyS0 >/dev/ttyS0 2>&1'; done
