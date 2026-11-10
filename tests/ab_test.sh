#!/bin/bash
# pruebas en PC de la tienda de Libros (audiolibros LibriVox / EPUB Gutenberg); no toca el aparato.
#   bash tests/ab_test.sh            parsers con las respuestas grabadas en tests/ab_samples
#   bash tests/ab_test.sh worker     + el proceso de descarga contra un servidor HTTP local
#                                    (cola, reanudacion con Range, pausa, espacio, reintentos);
#                                    necesita ~/hiby/net/hibynet-x86 o out/agent_audiobooks/hibynet-x86
set -e
H=${H:-$(cd "$(dirname "$0")/.." && pwd)}      # this checkout (or a worktree of it)
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-sign-compare -Wno-unused-parameter -Wno-format-truncation \
  -fsanitize=address,undefined -I$H/src -o /tmp/ab_test $H/tests/ab_test.c
/tmp/ab_test $H/tests/ab_samples
[ "$1" = worker ] || exit 0
NET=$H/out/agent_audiobooks/hibynet-x86; [ -x $NET ] || NET=$HOME/hiby/net/hibynet-x86
W=/tmp/ab_worker; rm -rf $W; mkdir -p $W/srv $W/bin $W/sd
cp $NET $W/bin/hibynet
printf '#!/bin/sh\ncp "$1" "$2"\n' > $W/bin/hibywall; chmod +x $W/bin/hibywall
head -c 3000000 /dev/urandom > $W/srv/a.mp3; head -c 1200000 /dev/urandom > $W/srv/b.mp3; head -c 5000 /dev/urandom > $W/srv/c.jpg
(cd $W/srv && exec python3 -m http.server 8799 --bind 127.0.0.1 >/dev/null 2>&1) & SP=$!
trap "kill $SP 2>/dev/null" EXIT
sleep 0.7
U=http://127.0.0.1:8799
printf "bk1\tm\t3000000\t$U/a.mp3\t$W/sd/Audiobooks/X - Y/01 - a.mp3\nbk1\tm\t1200000\t$U/b.mp3\t$W/sd/Audiobooks/X - Y/02 - b.mp3\nbk1\tc\t0\t$U/c.jpg|$W/c.cov\t$W/sd/Audiobooks/X - Y/cover.jpg\nbk2\tm\t5\t$U/nope.mp3\t$W/sd/Audiobooks/Z/01 - z.mp3\n" > $W/q
# un .part a medias: tiene que reanudarse (Range) y quedar identico
mkdir -p "$W/sd/Audiobooks/X - Y"; head -c 1000000 $W/srv/a.mp3 > "$W/sd/Audiobooks/X - Y/01 - a.mp3.part"
: > $W/paused
t0=$(date +%s.%N)
/tmp/ab_test --worker $W/q $W/paused $W/bin || true
echo "worker: $(echo "$(date +%s.%N) - $t0" | bc) s, estado: $(cat /tmp/hibymod/abdl.st | cut -f2-7)"
cmp $W/srv/a.mp3 "$W/sd/Audiobooks/X - Y/01 - a.mp3" && cmp $W/srv/b.mp3 "$W/sd/Audiobooks/X - Y/02 - b.mp3" && cmp $W/srv/c.jpg $W/c.cov && echo "descargas OK (reanudada + completa + portada)"
[ ! -e "$W/sd/Audiobooks/Z/01 - z.mp3" ] && grep -q bk2 /tmp/hibymod/abdl.err && echo "404: saltada tras los reintentos OK"
# pausa: un libro en pausa no se descarga
rm -f "$W/sd/Audiobooks/X - Y/02 - b.mp3"; echo bk1 > $W/paused; grep -v bk2 $W/q > $W/q2 || true; mv $W/q2 $W/q
/tmp/ab_test --worker $W/q $W/paused $W/bin; [ ! -e "$W/sd/Audiobooks/X - Y/02 - b.mp3" ] && echo "pausa OK"
# SIGTERM a mitad: el .part queda y la siguiente pasada termina
: > $W/paused; (HIBYNET_RATE=1 /tmp/ab_test --worker $W/q $W/paused $W/bin &) ; sleep 0.05; kill $(cat /tmp/hibymod/abdl.pid) 2>/dev/null || true; sleep 0.3
/tmp/ab_test --worker $W/q $W/paused $W/bin; cmp $W/srv/b.mp3 "$W/sd/Audiobooks/X - Y/02 - b.mp3" && echo "parar y seguir OK"
