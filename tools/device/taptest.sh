#!/bin/sh
getevent /dev/input/event1 > /tmp/ge.txt 2>&1 &
G=$!
sleep 0.5
/tmp/hibyinput tap 80 110
sleep 0.8
kill $G 2>/dev/null
echo "--- eventos vistos por un lector:"; cat /tmp/ge.txt | head -20
