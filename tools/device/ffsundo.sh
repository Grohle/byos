#!/bin/sh
# Deshacer la prueba ffs: dejar el gadget exactamente como estaba (solo almacenamiento).
G=/sys/kernel/config/usb_gadget/android0
[ -e $G/configs/c.1/ffs.adb ] && { echo "" > $G/UDC; rm $G/configs/c.1/ffs.adb; echo $(ls /sys/class/udc/) > $G/UDC; }
umount /dev/usb-ffs/adb 2>/dev/null
rmdir $G/functions/ffs.adb 2>/dev/null
rm -rf /dev/usb-ffs
echo "c.1: $(ls $G/configs/c.1 | tr '\n' ' ') | funciones: $(ls $G/functions | tr '\n' ' ') | UDC=[$(cat $G/UDC)]"
echo "=== funciones de red USB en el kernel"
for f in rndis ecm ncm eem subset; do
  if mkdir $G/functions/$f.probe 2>/dev/null; then echo "$f: SI"; rmdir $G/functions/$f.probe; else echo "$f: no"; fi
done
echo "=== applets de red"
for a in udhcpd ifconfig ip; do which $a >/dev/null 2>&1 && echo "$a: si" || echo "$a: no"; done
busybox 2>&1 | head -1
busybox --list 2>/dev/null | grep -E 'udhcpd|dhcpd|telnetd|httpd|nc$' | tr '\n' ' '
