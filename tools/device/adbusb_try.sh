#!/bin/sh
# Lanza el cambio a ADB por USB desligado de la sesion ADB actual (que se cortara)
( sleep 2; /usr/data/hibymod/bin/hibymod_adb.sh usb > /usr/data/hibymod/adbusb.log 2>&1; /usr/data/hibymod/bin/hibymod_adb.sh status >> /usr/data/hibymod/adbusb.log 2>&1 ) </dev/null >/dev/null 2>&1 &
echo lanzado
