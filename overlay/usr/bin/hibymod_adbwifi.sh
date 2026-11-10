#!/bin/sh
# hibymod: ADB por Wi-Fi (puerto 5555). Solo se ejecuta si el usuario lo activa.
# El adbd del firmware escucha por TCP cuando no existe la interfaz USB de ADB,
# asi que no interfiere con el modo almacenamiento USB.
# Comprueba cada 10 s si hay IP; no hace nada mas.

D=/tmp/hibymod
SD=/data/mnt/sd_0
mkdir -p $D

while [ -f /data/.hibymod_adbwifi ]
do
    ip=$(ip -4 addr show wlan0 2>/dev/null | awk '/inet /{sub(/\/.*/,"",$2); print $2; exit}')
    if [ -n "$ip" ] && ! pidof adbd >/dev/null && [ ! -e /dev/usb-ffs/adb/ep0 ] && [ ! -e /dev/android_adb ]; then
        /usr/bin/adbd >/dev/null 2>&1 &
        echo "$ip:5555" > $D/adb_wifi_ip
        if grep -qE " (/usr)?/data/mnt/sd_0 " /proc/mounts 2>/dev/null; then
            mkdir -p $SD/hibymod
            echo "adb connect $ip:5555" > $SD/hibymod/adb_wifi.txt
        fi
    fi
    sleep 10
done
