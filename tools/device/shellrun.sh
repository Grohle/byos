#!/bin/sh
# Arranca hibyshell en primer plano unos segundos para ver por que falla
/usr/data/hibymod/shell/hibyshell &
p=$!
sleep 3
if kill -0 $p 2>/dev/null; then echo "vivo (pid $p)"; else wait $p; echo "salio rc=$?"; fi
tail -4 /tmp/hibymod/shell.log
