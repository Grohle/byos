VM=~/hiby/vm; C="python3 $VM/hibyctl.py"
for u in "http://example.com/" "https://en.wikipedia.org/wiki/Telegram_(software)" "https://lite.cnn.com/" "https://text.npr.org/"; do
 echo "=== $u"
 $C sh "cd /root; HOME=/root sh /hiby/measure.sh 120 /usr/bin/netsurf-fb -f ram -w 320 -h 480 -b 16 '$u' | grep -vE '^t=' ; grep -E '^t=' /dev/null" 
done
