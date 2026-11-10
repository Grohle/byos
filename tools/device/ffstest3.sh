#!/bin/sh
( cd /; ADB_TRACE=1 /usr/bin/adbd > /tmp/adbd_usb.log 2>&1 & )
sleep 2
ls /dev/usb-ffs/adb
ps | grep adbd | grep -v grep
echo "--- log"; head -40 /tmp/adbd_usb.log; tail -15 /tmp/adbd_usb.log
