#!/bin/sh
# Prueba 1: crear la funcion ffs.adb (sin enlazarla a la configuracion USB) y montar functionfs.
G=/sys/kernel/config/usb_gadget/android0
mkdir $G/functions/ffs.adb 2>&1 && echo "ffs.adb creada" || echo "mkdir ffs.adb FALLA"
grep functionfs /proc/filesystems || echo "functionfs NO registrado"
mkdir -p /dev/usb-ffs/adb
mount -t functionfs adb /dev/usb-ffs/adb 2>&1 && echo "functionfs montado" || echo "mount functionfs FALLA"
ls -la /dev/usb-ffs/adb
