VM=~/hiby/vm; C="python3 $VM/hibyctl.py"
pkill -f qemu-system-mipsel; sleep 1
rm -f $VM/logs/serial.log
setsid timeout 300 $VM/run_vm.sh $@ >/dev/null 2>&1 &
for i in $(seq 1 60); do sleep 1; grep -q "HIBY-VM READY" $VM/logs/serial.log 2>/dev/null && break; done
grep -E "Mem|event" $VM/logs/serial.log
$C fbinfo
$C sh '/hiby/bin/evdump /dev/input/event1 > /tmp/ev1.txt & /hiby/bin/evdump /dev/input/event3 > /tmp/ev3.txt & /hiby/bin/fbtouch > /tmp/fbt.txt 2>&1 & sleep 1; echo started'
$C tap 160 240
$C swipe 20 100 300 400 200
$C key VOLUMEUP
$C key POWER
$C sh 'sleep 1; cat /tmp/ev1.txt | head -24; echo; cat /tmp/ev3.txt; echo; cat /tmp/fbt.txt | head; echo; tail -3 /tmp/fbt.txt'
$C shot $VM/out/fbtouch.png
$C sh 'free; cat /proc/cpuinfo | head -8'
