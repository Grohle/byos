#!/bin/bash
# End-to-end PC test of the radio stream helper ("hibynet radio", src/hibynet_radio.inc.c) against a
# local fake station server (tests/radio_helper_server.py) and a fake player process
# (tests/radio_fake_player.c). No Internet, no device. Needs the x86 build deps of
# tools/wsl/zbuild_net.sh x86 (~/hiby/net/x86).
#   1. redirect + ICY + junk before the first frame, a reader that plays, pauses and closes:
#      ready/open/title/codec, tmpfs usage bounded (punched holes), stops by itself, no files left
#   2. .pls with a dead first entry; SIGTERM cleans up
#   3. .m3u -> chunked AAC
#   4. refused: HLS, Ogg, HTTP 404
#   5. the server drops the connection: the helper reconnects
# bash tests/radio_helper_test.sh
set -e
H=${H:-$(cd "$(dirname "$0")/.." && pwd)}
N=$HOME/hiby/net
B=/tmp/radio_helper_test; mkdir -p $B
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-format-truncation -I$HOME/hiby/third -I$N/x86/include -o $B/hibynet $H/src/hibynet.c \
  -L$N/x86/lib -lmbedtls -lmbedx509 -lmbedcrypto -ljpeg -lm
gcc -O1 -Wall -Wextra -o $B/rt_player $H/tests/radio_fake_player.c
PORT=$((20000 + RANDOM % 20000))
python3 $H/tests/radio_helper_server.py $PORT & SRV=$!
D=/dev/shm/radio_helper_test.$$          # tmpfs, like /tmp on the player
mkdir -p $D
PIDS=""
cleanup() { kill $SRV $PIDS 2>/dev/null || true; [ -n "$KEEP" ] && cp -r $D $B/last; rm -rf $D; }
trap cleanup EXIT
sleep 0.7
U=http://127.0.0.1:$PORT
fails=0
ok() { echo "  ok: $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }
st() { grep "^$2=" $1/state 2>/dev/null | head -1 | cut -d= -f2-; }
waitst() { for i in $(seq 1 $(($3 * 10))); do [ "$(st $1 st)" = "$2" ] && return 0; sleep 0.1; done; return 1; }
alloc_kb() { local f="$(st $1 file)"; [ -n "$f" ] && [ -f "$f" ] && echo $(( $(stat -c %b "$f") * $(stat -c %B "$f") / 1024 )) || echo 0; }

echo "== 1. redirect, ICY, reader plays / pauses / closes"
mkdir -p $D/a
HIBYRADIO_PLAYER=rt_player HIBYRADIO_HIGH_KB=256 HIBYNET_VERBOSE=1 $B/hibynet radio $U/redir $D/a "Test FM" 256 2> $B/a.log & HP=$!; PIDS="$PIDS $HP"
$B/rt_player $D/a/state 16000 10 8 > $B/p.log & RP=$!; PIDS="$PIDS $RP"
if waitst $D/a ready 10; then ok "ready"; else bad "never ready (st=$(st $D/a st) err=$(st $D/a err))"; fi
[ "$(st $D/a codec)" = MP3 ] && [ "$(st $D/a br)" = 128 ] && ok "MP3 128k" || bad "codec/br $(st $D/a codec) $(st $D/a br)"
F="$(st $D/a file)"
[ "$F" = "$D/a/Test FM.mp3" ] && ok "file name" || bad "file $F"
[ "$(stat -c %s "$F")" = $((256 << 20)) ] && ok "sparse file, 256 MB logical" || bad "size $(stat -c %s "$F")"
for i in $(seq 1 30); do [ "$(st $D/a open)" = 1 ] && break; sleep 0.1; done
[ "$(st $D/a open)" = 1 ] && ok "player opened it (open=1)" || bad "open=$(st $D/a open)"
head -c 4 "$F" | od -An -tx1 | grep -q "ff fb 90" && ok "file starts on a whole frame" || bad "first bytes $(head -c 4 "$F" | od -An -tx1)"
max=0
for i in $(seq 1 16); do k=$(alloc_kb $D/a); [ $k -gt $max ] && max=$k; sleep 1; done
case "$(st $D/a title)" in "Test Artist - Song "*) ok "ICY title: $(st $D/a title)";; *) bad "title '$(st $D/a title)'";; esac
[ $max -le 600 ] && ok "tmpfs use bounded while playing and paused (max ${max} KB, cap 256 KB ahead)" || bad "tmpfs use ${max} KB"
grep -q "closed" $B/p.log || sleep 2
grep -q "closed gens=1 " $B/p.log && ok "no underrun (one generation)" || bad "player: $(grep closed $B/p.log)"
if waitst $D/a stopped 8; then ok "player closed the file: helper stopped by itself"; else bad "still st=$(st $D/a st)"; fi
sleep 0.5
kill -0 $HP 2>/dev/null && bad "helper still running" || ok "helper exited"
ls $D/a | grep -qE '\.(mp3|aac)$' && bad "audio left behind: $(ls $D/a)" || ok "no audio file left"
[ -e $D/a/pid ] && bad "pid file left" || ok "no pid file left"

echo "== 2. .pls with a dead first entry, then SIGTERM"
mkdir -p $D/b
HIBYRADIO_NOREADER=1 $B/hibynet radio $U/list.pls $D/b "Pls FM" 2>/dev/null & HP=$!; PIDS="$PIDS $HP"
if waitst $D/b ready 10; then ok "ready through the playlist"; else bad "pls st=$(st $D/b st) err=$(st $D/b err)"; fi
kill -TERM $HP; sleep 0.5
kill -0 $HP 2>/dev/null && bad "SIGTERM ignored" || ok "SIGTERM: exited"
[ "$(st $D/b st)" = stopped ] && ok "state stopped" || bad "state $(st $D/b st)"
ls $D/b | grep -qE '\.(mp3|aac)$' && bad "audio left behind" || ok "no audio file left"

echo "== 3. .m3u -> chunked AAC"
mkdir -p $D/c
HIBYRADIO_NOREADER=1 $B/hibynet radio $U/list.m3u $D/c "Aac" 2>/dev/null & HP=$!; PIDS="$PIDS $HP"
if waitst $D/c ready 10; then ok "ready"; else bad "aac st=$(st $D/c st) err=$(st $D/c err)"; fi
[ "$(st $D/c codec)" = AAC ] && ok "AAC" || bad "codec $(st $D/c codec)"
case "$(st $D/c file)" in *.aac) ok ".aac file";; *) bad "file $(st $D/c file)";; esac
head -c 2 "$(st $D/c file)" | od -An -tx1 | grep -q "ff f1" && ok "ADTS frame first (chunk headers removed)" || bad "aac first bytes"
kill -TERM $HP; sleep 0.3

echo "== 4. refused streams"
for t in "hls.m3u8 hls" "ogg codec" "dead http 404"; do
  set -- $t; p=$1; shift; want="$*"; mkdir -p $D/r_$p
  timeout 20 $B/hibynet radio $U/$p $D/r_$p "R" 2>/dev/null || true
  [ "$(st $D/r_$p st)" = error ] && [ "$(st $D/r_$p err)" = "$want" ] && ok "$p -> err=$want" || bad "$p: st=$(st $D/r_$p st) err=$(st $D/r_$p err)"
  ls $D/r_$p | grep -qE '\.(mp3|aac)$' && bad "$p: audio left behind" || true
done

echo "== 5. connection dropped: reconnects"
mkdir -p $D/e
HIBYRADIO_NOREADER=1 HIBYNET_VERBOSE=1 $B/hibynet radio $U/drop $D/e "Drop" 2> $D/e.log & HP=$!; PIDS="$PIDS $HP"
sleep 9
n=$(grep -c "radio: GET" $D/e.log || true)
[ $n -ge 2 ] && ok "reconnected ($n requests)" || bad "requests: $n"
kill -0 $HP 2>/dev/null && ok "still running" || bad "gave up: st=$(st $D/e st) err=$(st $D/e err)"
kill -TERM $HP 2>/dev/null || true; sleep 0.3

echo "== 6. a reader faster than the stream: underrun, new generation, recovers; then other music"
mkdir -p $D/f
head -c 4096 /dev/zero > $B/other.flac
HIBYRADIO_PLAYER=rt_player HIBYRADIO_HIGH_KB=256 HIBYNET_VERBOSE=1 $B/hibynet radio $U/icy $D/f "Fast" 256 2> $B/f.log & HP=$!; PIDS="$PIDS $HP"
$B/rt_player $D/f/state 60000 17 0 $B/other.flac > $B/pf.log & RP=$!; PIDS="$PIDS $RP"
for i in $(seq 1 300); do grep -q "^closed" $B/pf.log && break; sleep 0.1; done
g=$(grep -o "closed gens=[0-9]*" $B/pf.log | cut -d= -f2)
[ -n "$g" ] && [ $g -ge 2 ] && ok "player followed $g generations" || bad "generations: '$g' ($(tail -1 $B/pf.log))"
grep -q "corte: el lector alcanzo" $B/f.log && ok "underrun detected" || bad "no underrun logged"
if waitst $D/f stopped 6; then ok "stopped when the player switched to other music (phase: $(grep -c 'lista' $B/f.log) announcements)"; else bad "st=$(st $D/f st) err=$(st $D/f err)"; fi
sleep 0.5; ls $D/f | grep -qE '\.(mp3|aac)$' && bad "audio left behind: $(ls $D/f)" || ok "no audio file left (old generations too)"

echo "radio helper: $fails failures"
[ $fails = 0 ]
