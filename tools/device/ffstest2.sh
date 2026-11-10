#!/bin/sh
# Prueba 2: adbd en modo USB + funcion ADB enlazada al gadget de almacenamiento (compuesto).
G=/sys/kernel/config/usb_gadget/android0
UDC=$(ls /sys/class/udc/)
# adbd detecta /dev/usb-ffs/adb/ep0 y arranca en modo USB (el adbd de Wi-Fi sigue aparte)
start-stop-daemon -S -b -x /usr/bin/adbd 2>/dev/null || (/usr/bin/adbd </dev/null >/dev/null 2>&1 &)
i=0; while [ $i -lt 30 ] && [ ! -e /dev/usb-ffs/adb/ep1 ]; do sleep 0.1; i=$((i+1)); done
ls /dev/usb-ffs/adb
[ -e /dev/usb-ffs/adb/ep1 ] || { echo "adbd no escribio descriptores"; exit 1; }
echo "UDC antes=[$(cat $G/UDC)]"
echo "" > $G/UDC
[ -e $G/configs/c.1/ffs.adb ] || ln -s $G/functions/ffs.adb $G/configs/c.1/ffs.adb
echo $UDC > $G/UDC
echo "UDC despues=[$(cat $G/UDC)] funciones en c.1: $(ls $G/configs/c.1 | tr '\n' ' ')"
ps | grep adbd | grep -v grep
