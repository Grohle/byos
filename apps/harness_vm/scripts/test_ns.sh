VM=~/hiby/vm; C="python3 $VM/hibyctl.py"
pkill -f qemu-system-mipsel; sleep 1; rm -f $VM/logs/serial.log
setsid timeout 900 $VM/run_vm.sh "$@" >/dev/null 2>&1 &
for i in $(seq 1 60); do sleep 1; grep -q "HIBY-VM READY" $VM/logs/serial.log 2>/dev/null && break; done
grep -E "mem_profile|MemAvail" $VM/logs/serial.log
$C sh 'ls /usr/bin/netsurf* /usr/share/netsurf | head; netsurf-fb --help 2>&1 | head -30; ldd /usr/bin/netsurf-fb | wc -l; ldd /usr/bin/netsurf-fb | grep -iE "sdl|x11|xcb|vnc|freetype|curl|ssl"'
$C sh 'wget -q -O- http://example.com | head -3'
