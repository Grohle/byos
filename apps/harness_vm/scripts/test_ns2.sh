VM=~/hiby/vm; C="python3 $VM/hibyctl.py"
$C sh 'LD_TRACE_LOADED_OBJECTS=1 /usr/bin/netsurf-fb | wc -l; LD_TRACE_LOADED_OBJECTS=1 /usr/bin/netsurf-fb | grep -iE "sdl|x11|xcb|vnc|freetype|curl|ssl|nsfb"; ls -la /usr/bin/netsurf-fb'
$C sh 'cd /root; HOME=/root sh /hiby/measure.sh 150 /usr/bin/netsurf-fb -f linux -w 320 -h 480 -b 16 http://example.com/'
$C shot $VM/out/ns_example.png
