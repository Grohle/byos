#!/bin/sh
# Tiempo medio por ejecucion (20 repeticiones) de distintos comandos.
t() {
  t0=$(cut -d' ' -f1 /proc/uptime)
  i=0; while [ $i -lt 20 ]; do "$@" > /dev/null 2>&1; i=$((i+1)); done
  t1=$(cut -d' ' -f1 /proc/uptime)
  echo "$(echo "$t0 $t1" | awk '{printf "%5.1f ms", ($2-$1)*1000/20}')  $*"
}
t /tmp/hibyctl info
t /tmp/fbin/wpa_cli -i wlan0 signal_poll
t /usr/sbin/wpa_cli -i wlan0 signal_poll
t sh -c "/tmp/fbin/wpa_cli -i wlan0 signal_poll > /tmp/x"
t sh -c "wpa_cli -i wlan0 signal_poll > /tmp/x"
