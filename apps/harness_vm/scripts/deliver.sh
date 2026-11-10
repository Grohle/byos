set -e
VM=~/hiby/vm; D=/mnt/c/Users/alber/Downloads/Hiby/hibymod/apps
mkdir -p $D/netsurf-hiby $D/fbtouch $D/harness_vm/{tools,guest,scripts,kernel}
cp -r $VM/share/netsurf/. $D/netsurf-hiby/
cp $VM/out/bin/fbtouch $VM/tools/fbtouch.c $D/fbtouch/
cp $VM/tools/*.c $D/harness_vm/tools/
cp $VM/guest/*.sh $D/harness_vm/guest/
cp $VM/scripts/*.sh $VM/scripts/*.py $VM/scripts/*.cmds $D/harness_vm/scripts/
cp $VM/run_vm.sh $VM/hibyctl.py $D/harness_vm/
cp $VM/out/kernel.config $D/harness_vm/kernel/config-6.1.192-hiby
cp $VM/out/bin/hibyinput $VM/out/bin/evdump $VM/out/bin/memhog $D/harness_vm/tools/
find $D -type f | wc -l; du -sh $D
