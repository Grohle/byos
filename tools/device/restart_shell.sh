#!/bin/sh
# Instala hibyshell.new si existe y relanza el shell (sin depender del supervisor)
D=/usr/data/hibymod/shell
if [ -f $D/hibyshell.new ]; then chmod 755 $D/hibyshell.new; mv $D/hibyshell.new $D/hibyshell; fi
echo 0 > $D/starts   # un reinicio de desarrollo no es un cierre (auto-reparacion)
killall hibyshell 2>/dev/null
sleep 1
echo 0 > $D/starts
sleep 4; pidof hibyshell >/dev/null || setsid $D/hibyshell </dev/null >/dev/null 2>&1 &
sleep 3
pidof hibyshell
tail -2 /tmp/hibymod/shell.log
