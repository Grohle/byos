#!/bin/sh
echo "=== quien llama a usb_dev_mass_storage"
for b in /usr/bin/sys_server /usr/bin/hiby_player; do
  echo "-- $b"; strings $b 2>/dev/null | grep -E 'usb_dev_mass_storage|usb_storage_init|mmcblk|UDC|adbon|android0' | sort -u
done
echo "=== gadget"
ls -la /sys/kernel/config/usb_gadget/ 2>&1
for g in /sys/kernel/config/usb_gadget/*; do
  [ -d "$g" ] || continue
  echo "-- $g UDC=[$(cat $g/UDC)] vid=$(cat $g/idVendor) pid=$(cat $g/idProduct)"
  ls $g/functions $g/configs/c.1 2>&1
  ls $g/os_desc 2>&1
done
echo "=== udc"; ls /sys/class/udc/; cat /sys/class/udc/*/state
echo "=== functionfs"; grep -E 'functionfs|configfs' /proc/filesystems
ls -la /dev/usb-ffs 2>&1
echo "=== procesos adbd"; ps | grep -E 'adbd|adbserver' | grep -v grep
