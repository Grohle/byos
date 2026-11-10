VM=~/hiby/vm
ls -la $VM/rootfs/lib/ld.so.1 $VM/rootfs/lib/mipsel-linux-gnu/ld.so.1 2>&1 | head
sed -i 's/\r$//' $VM/run_vm.sh; chmod +x $VM/run_vm.sh
rm -f $VM/logs/serial.log
timeout 120 $VM/run_vm.sh &
for i in $(seq 1 110); do sleep 1; grep -q "HIBY-VM READY" $VM/logs/serial.log 2>/dev/null && break; done
echo "boot secs: $i"
cat $VM/logs/serial.log | tail -30
