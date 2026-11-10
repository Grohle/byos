#!/bin/sh
# hibymod_adb.sh off|wifi|usb|status  -  modo de ADB (solo puede haber un adbd: o Wi-Fi o USB)
#  wifi: adbd escucha en TCP 5555 (como hasta ahora; se reintenta hasta que haya IP)
#  usb : funcion ADB (functionfs) anadida al gadget USB junto al almacenamiento: el PC ve
#        el disco y "ADB Interface". La primera vez hay un periodo de prueba: si en 90 s
#        nadie abre una sesion ADB por USB, se vuelve a Wi-Fi (para no quedarse sin acceso).
G=/sys/kernel/config/usb_gadget/android0
SELF=$(readlink -f "$0")
M=/tmp/hibymod
mkdir -p $M

usb_down() {
    if [ -e $G/configs/c.1/ffs.adb ]; then
        echo "" > $G/UDC; rm -f $G/configs/c.1/ffs.adb; ls /sys/class/udc/ > $G/UDC
    fi
    umount /dev/usb-ffs/adb 2>/dev/null
    rmdir $G/functions/ffs.adb 2>/dev/null
    rm -rf /dev/usb-ffs
}
stop_adbd() { killall adbd 2>/dev/null; sleep 1; killall -9 adbd 2>/dev/null; }

wifi_on() {
    echo wifi > /data/.hibymod_adbmode; touch /data/.hibymod_adbwifi; sync
    stop_adbd; usb_down
    pidof hibymod_adbwifi.sh >/dev/null || (setsid /usr/bin/hibymod_adbwifi.sh </dev/null >/dev/null 2>&1 &)
}

usb_on() {
    # se mantiene la marca de Wi-Fi como respaldo: al arrancar, si el USB no esta listo, adbd va por Wi-Fi
    echo usb > /data/.hibymod_adbmode; touch /data/.hibymod_adbwifi; sync
    stop_adbd; usb_down
    mkdir $G/functions/ffs.adb || { echo "sin functionfs"; wifi_on; return 1; }
    mkdir -p /dev/usb-ffs/adb
    mount -t functionfs adb /dev/usb-ffs/adb || { echo "no se pudo montar functionfs"; wifi_on; return 1; }
    (cd /; setsid /usr/bin/adbd </dev/null >/dev/null 2>&1 &)
    i=0; while [ $i -lt 50 ] && [ ! -e /dev/usb-ffs/adb/ep1 ]; do sleep 0.1; i=$((i + 1)); done
    [ -e /dev/usb-ffs/adb/ep1 ] || { echo "adbd no arranco en modo USB"; wifi_on; return 1; }
    echo "" > $G/UDC
    ln -s $G/functions/ffs.adb $G/configs/c.1/ffs.adb
    ls /sys/class/udc/ > $G/UDC
    echo "usb activo"
    if [ ! -f /data/.hibymod_adbusb_ok ]; then
        (   # periodo de prueba: una sesion por USB (un proceso hijo de adbd) lo confirma
            n=0
            while [ $n -lt 90 ]; do
                p=$(pidof adbd)
                for c in /proc/[0-9]*/stat; do
                    [ -n "$p" ] && [ "$(cut -d' ' -f4 $c 2>/dev/null)" = "$p" ] && { touch /data/.hibymod_adbusb_ok; sync; exit 0; }
                done
                sleep 1; n=$((n + 1))
            done
            [ "$(cat /data/.hibymod_adbmode 2>/dev/null)" = usb ] && "$SELF" wifi
        ) </dev/null >/dev/null 2>&1 &
    fi
}

case "$1" in
    off)  echo off > /data/.hibymod_adbmode; rm -f /data/.hibymod_adbwifi; sync; stop_adbd; usb_down; echo "adb desactivado" ;;
    wifi) wifi_on; echo "adb wifi" ;;
    usb)  usb_on ;;
    status)
        echo "modo: $(cat /data/.hibymod_adbmode 2>/dev/null || ([ -f /data/.hibymod_adbwifi ] && echo wifi || echo off))"
        echo "adbd: $(pidof adbd)  usb-ffs: $([ -e /dev/usb-ffs/adb/ep1 ] && echo si || echo no)  verificado-usb: $([ -f /data/.hibymod_adbusb_ok ] && echo si || echo no)"
        echo "ip: $(ip -4 addr show wlan0 2>/dev/null | awk '/inet /{sub(/\/.*/,"",$2); print $2; exit}')"
        echo "gadget: $(ls $G/configs/c.1 | tr '\n' ' ') udc=$(cat $G/UDC) estado=$(cat /sys/class/udc/*/state)"
        ;;
    *) echo "uso: hibymod_adb.sh off|wifi|usb|status"; exit 1 ;;
esac
