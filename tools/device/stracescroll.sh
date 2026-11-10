#!/bin/sh
# Llamadas al sistema lentas (>10 ms) de todos los hilos de hiby_player mientras se desplaza la lista.
P=$(pidof hiby_player)
# deshacer la prueba de prioridades
for t in /proc/$P/task/*; do renice -n 0 -p ${t##*/} >/dev/null 2>&1; done
strace -f -T -e trace=open,openat,read,pread64,stat64,lstat64,fstat64,getdents64,ioctl,write,fsync,fdatasync,mmap2,munmap -p $P -o /tmp/st.txt &
S=$!
sleep 1
for i in 1 2 3; do /tmp/hibyctl swipe 160 420 160 80 250; sleep 0.6; done
sleep 0.5
kill $S; sleep 0.5
echo "lineas: $(wc -l < /tmp/st.txt)"
echo "--- llamadas > 10 ms"
awk -F'<' '/<[0-9.]+>$/{t=$NF; sub(">","",t); if (t+0 > 0.010) print t, $0}' /tmp/st.txt | sort -rn | head -25
echo "--- ficheros abiertos durante el desplazamiento"
grep -oE 'open(at)?\([^"]*"[^"]+"' /tmp/st.txt | sed 's/.*"\(.*\)"/\1/' | sort | uniq -c | sort -rn | head -20
