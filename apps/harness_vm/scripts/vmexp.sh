#!/bin/bash
# vmexp.sh LOG PROFILE CMDFILE : arranca VM con perfil, ejecuta cada linea de CMDFILE en el guest (sh), guarda en LOG.
VM=~/hiby/vm; C="python3 $VM/hibyctl.py"; LOG=$1; PROF=$2; CMDS=$3; shift 3
pkill -f qemu-system-mipsel; sleep 1; rm -f $VM/logs/serial.log
setsid timeout 3600 $VM/run_vm.sh --profile $PROF "$@" >/dev/null 2>&1 &
for i in $(seq 1 60); do sleep 1; grep -q "HIBY-VM READY" $VM/logs/serial.log 2>/dev/null && break; done
{ echo "### profile=$PROF args=$*"; grep -E "mem_profile|MemTotal" $VM/logs/serial.log
while IFS= read -r line; do
  [ -z "$line" ] && continue
  case "$line" in \#*) continue;; esac
  case "$line" in
    HOST:*) eval "${line#HOST:}";;
    *) echo ">>> $line"; $C sh "$line";;
  esac
done < $CMDS; } > $LOG 2>&1
grep -c "nobody cared" $VM/logs/serial.log >> $LOG
pkill -f qemu-system-mipsel
