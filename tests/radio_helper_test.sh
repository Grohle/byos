#!/bin/bash
# End-to-end PC test of the radio stream helper ("hibynet radio", src/hibynet_radio.inc.c) against a
# local fake station server (tests/radio_helper_server.py) and a fake player process
# (tests/radio_fake_player.c). No Internet, no device. Needs the x86 build deps of
# tools/wsl/zbuild_net.sh x86 (~/hiby/net/x86).
# The audio goes to DIR (the SD card on the player; here a plain folder) and the state file to
# STATEDIR (tmpfs on the player), as the shell runs it.
#   1. redirect + ICY + junk before the first frame, a reader that plays, pauses and closes:
#      the lead (LEAD_S of audio) is written before the file is announced and the reader never
#      catches up; ready/open/title/codec; the file grows (no sparse file); stops by itself; no
#      files left
#   2. .pls with a dead first entry; SIGTERM cleans up
#   3. .m3u -> chunked AAC
#   4. refused: Ogg, HTTP 404, encrypted HLS, fMP4 HLS
#   5. the server drops the connection: the helper reconnects
#   6. a reader faster than the stream that stops at the end of the file (as the stock engine
#      does): a new generation (new file, new pre-buffer), the player follows; then other music
#   7. HLS: master -> lightest audio variant, live MPEG-TS playlist that advances, relative and
#      absolute URIs, keep-alive; the frames come out in order, without holes or repeats
#   8. HLS packed AAC (two ID3 tags per segment) and a gzip-compressed playlist
#   9. session cap: stops cleanly with err=cap when the reader reaches the end
#  10. no SD card mounted: err=nosd, nothing written
#  11. the shell dies: the helper exits and deletes its files
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
D=/dev/shm/radio_helper_test.$$
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
noaudio() { ! ls "$1" 2>/dev/null | grep -qE '\.(mp3|aac)$'; }
# helper: radio URL NAME DIR [LEAD_S] [CAP_MB]; the state goes to DIR/st
radio() { local u=$1 n=$2 d=$3; mkdir -p $d; $B/hibynet radio "$u" $d "$n" ${4:-15} ${5:-128} $d/st; }

echo "== 1. redirect, ICY, lead before the announcement, reader plays / pauses / closes"
HIBYRADIO_PLAYER=rt_player HIBYRADIO_HIGH_S=30 HIBYNET_VERBOSE=1 radio $U/redir "Test FM" $D/a 6 2> $B/a.log & HP=$!; PIDS="$PIDS $HP"
$B/rt_player $D/a/st/state 16000 14 6 > $B/p.log & RP=$!; PIDS="$PIDS $RP"
if waitst $D/a/st ready 15; then ok "ready"; else bad "never ready (st=$(st $D/a/st st) err=$(st $D/a/st err))"; fi
[ "$(st $D/a/st codec)" = MP3 ] && [ "$(st $D/a/st br)" = 128 ] && ok "MP3 128k" || bad "codec/br $(st $D/a/st codec) $(st $D/a/st br)"
F="$(st $D/a/st file)"
[ "$F" = "$D/a/Test FM.mp3" ] && ok "audio in DIR, state in STATEDIR" || bad "file $F"
for i in $(seq 1 30); do grep -q "^opened" $B/p.log && break; sleep 0.1; done
s0=$(grep -m1 "^opened" $B/p.log | sed 's/.*size=//')
[ -n "$s0" ] && [ $s0 -ge $((6 * 16000 * 95 / 100)) ] && ok "announced with >= 6 s written ($s0 bytes)" || bad "announced with $s0 bytes"
for i in $(seq 1 30); do [ "$(st $D/a/st open)" = 1 ] && break; sleep 0.1; done
[ "$(st $D/a/st open)" = 1 ] && ok "player opened it (open=1)" || bad "open=$(st $D/a/st open)"
head -c 4 "$F" | od -An -tx1 | grep -q "ff fb 90" && ok "file starts on a whole frame" || bad "first bytes $(head -c 4 "$F" | od -An -tx1)"
sleep 4
sz=$(stat -c %s "$F"); al=$(( $(stat -c %b "$F") * $(stat -c %B "$F") ))
[ $sz -gt $s0 ] && [ $al -le $((sz + 65536)) ] && ok "plain growing file ($sz bytes, no sparse size)" || bad "size $sz allocated $al"
case "$(st $D/a/st title)" in "Test Artist - Song "*) ok "ICY title: $(st $D/a/st title)";; *) bad "title '$(st $D/a/st title)'";; esac
grep -q "closed" $B/p.log || sleep 12
grep -q "closed" $B/p.log || sleep 4
mh=$(grep -o "minahead=[-0-9]*" $B/p.log | cut -d= -f2)
[ -n "$mh" ] && [ $mh -ge 16000 ] && ok "the reader never caught up (at least $mh bytes ahead, > 1 s)" || bad "minahead '$mh'"
grep -q "closed gens=1 " $B/p.log && ok "no underrun (one generation)" || bad "player: $(grep closed $B/p.log)"
if waitst $D/a/st stopped 8; then ok "player closed the file: helper stopped by itself"; else bad "still st=$(st $D/a/st st)"; fi
sleep 0.5
kill -0 $HP 2>/dev/null && bad "helper still running" || ok "helper exited"
noaudio $D/a && ok "no audio file left" || bad "audio left behind: $(ls $D/a)"
[ -e $D/a/st/pid ] && bad "pid file left" || ok "no pid file left"

echo "== 2. .pls with a dead first entry, then SIGTERM"
HIBYRADIO_NOREADER=1 radio $U/list.pls "Pls FM" $D/b 4 2>/dev/null & HP=$!; PIDS="$PIDS $HP"
if waitst $D/b/st ready 12; then ok "ready through the playlist"; else bad "pls st=$(st $D/b/st st) err=$(st $D/b/st err)"; fi
kill -TERM $HP; sleep 0.5
kill -0 $HP 2>/dev/null && bad "SIGTERM ignored" || ok "SIGTERM: exited"
[ "$(st $D/b/st st)" = stopped ] && ok "state stopped" || bad "state $(st $D/b/st st)"
noaudio $D/b && ok "no audio file left" || bad "audio left behind"

echo "== 3. .m3u -> chunked AAC"
HIBYRADIO_NOREADER=1 radio $U/list.m3u "Aac" $D/c 4 2>/dev/null & HP=$!; PIDS="$PIDS $HP"
if waitst $D/c/st ready 12; then ok "ready"; else bad "aac st=$(st $D/c/st st) err=$(st $D/c/st err)"; fi
[ "$(st $D/c/st codec)" = AAC ] && ok "AAC" || bad "codec $(st $D/c/st codec)"
case "$(st $D/c/st file)" in *.aac) ok ".aac file";; *) bad "file $(st $D/c/st file)";; esac
head -c 2 "$(st $D/c/st file)" | od -An -tx1 | grep -q "ff f1" && ok "ADTS frame first (chunk headers removed)" || bad "aac first bytes"
kill -TERM $HP; sleep 0.3

echo "== 4. refused streams"
for t in "ogg codec" "dead http 404" "hls/enc.m3u8 enc" "hls/fmp4.m3u8 codec"; do
  set -- $t; p=$1; shift; want="$*"; d=$D/r_$(echo $p | tr '/.' '__')
  mkdir -p $d; timeout 25 $B/hibynet radio $U/$p $d R 15 128 $d/st 2>/dev/null || true
  [ "$(st $d/st st)" = error ] && [ "$(st $d/st err)" = "$want" ] && ok "$p -> err=$want" || bad "$p: st=$(st $d/st st) err=$(st $d/st err)"
  noaudio $d || bad "$p: audio left behind"
done

echo "== 5. connection dropped: reconnects"
HIBYRADIO_NOREADER=1 HIBYNET_VERBOSE=1 radio $U/drop "Drop" $D/e 4 2> $D/e.log & HP=$!; PIDS="$PIDS $HP"
sleep 9
n=$(grep -c "radio: GET" $D/e.log || true)
[ $n -ge 2 ] && ok "reconnected ($n requests)" || bad "requests: $n"
kill -0 $HP 2>/dev/null && ok "still running" || bad "gave up: st=$(st $D/e/st st) err=$(st $D/e/st err)"
kill -TERM $HP 2>/dev/null || true; sleep 0.3

echo "== 6. a reader faster than the stream stops at the end of the file: new generation; then other music"
head -c 4096 /dev/zero > $B/other.flac
HIBYRADIO_PLAYER=rt_player HIBYNET_VERBOSE=1 radio $U/icy "Fast" $D/f 3 2> $B/f.log & HP=$!; PIDS="$PIDS $HP"
FAKE_EOFSTOP=1 $B/rt_player $D/f/st/state 60000 26 0 $B/other.flac > $B/pf.log & RP=$!; PIDS="$PIDS $RP"
for i in $(seq 1 400); do grep -q "^closed" $B/pf.log && break; sleep 0.1; done
g=$(grep -o "closed gens=[0-9]*" $B/pf.log | cut -d= -f2)
grep -q "^eof" $B/pf.log && ok "the player stopped at the end of the file ($(grep -c '^eof' $B/pf.log) times)" || bad "no eof: $(tail -2 $B/pf.log)"
[ -n "$g" ] && [ $g -ge 2 ] && ok "player followed $g generations" || bad "generations: '$g' ($(tail -1 $B/pf.log))"
grep -q "corte: el lector alcanzo" $B/f.log && ok "underrun detected, clean restart" || bad "no underrun logged"
if waitst $D/f/st stopped 8; then ok "stopped when the player switched to other music"; else bad "st=$(st $D/f/st st) err=$(st $D/f/st err)"; fi
sleep 0.5; noaudio $D/f && ok "no audio file left (old generations too)" || bad "audio left behind: $(ls $D/f)"

echo "== 7. HLS: master playlist, live MPEG-TS, sequence tracking, keep-alive"
c0=$(curl -s $U/hls/stats)
HIBYRADIO_PLAYER=rt_player HIBYNET_VERBOSE=1 radio $U/hls/master.m3u8 "Hls" $D/h 10 2> $B/h.log & HP=$!; PIDS="$PIDS $HP"
$B/rt_player $D/h/st/state 9375 13 0 > $B/ph.log & RP=$!; PIDS="$PIDS $RP"
if waitst $D/h/st ready 15; then ok "ready"; else bad "hls st=$(st $D/h/st st) err=$(st $D/h/st err)"; fi
[ "$(st $D/h/st codec)" = AAC ] && [ "$(st $D/h/st hls)" = 1 ] && ok "AAC from HLS" || bad "codec $(st $D/h/st codec) hls=$(st $D/h/st hls)"
grep -q "lista maestra.*/hls/lo/index.m3u8" $B/h.log && ok "master: the lightest audio-only variant (lo, not video nor hi)" || bad "variant: $(grep maestra $B/h.log)"
F="$(st $D/h/st file)"
sleep 8
cp "$F" $B/h.aac 2>/dev/null || true
r=$(python3 $H/tests/radio_frames_check.py $B/h.aac)
case "$r" in *ok=1*) ok "frames in order, no holes or repeats across segments: $r";; *) bad "frames: $r";; esac
grep -q "seg_.*\.ts (misma conexion)" $B/h.log && grep -q "GET http://127.0.0.1:$PORT/hls/lo/seg_" $B/h.log && ok "relative and absolute segment URIs" || bad "segment requests"
c1=$(curl -s $U/hls/stats)
conns=$(( $(echo $c1 | sed 's/conns=\([0-9]*\).*/\1/') - $(echo $c0 | sed 's/conns=\([0-9]*\).*/\1/') ))
reqs=$(( $(echo $c1 | sed 's/.*reqs=\([0-9]*\).*/\1/') - $(echo $c0 | sed 's/.*reqs=\([0-9]*\).*/\1/') ))
[ $reqs -ge 6 ] && [ $conns -le 3 ] && ok "keep-alive: $reqs requests over $conns connections" || bad "keep-alive: $reqs requests, $conns connections"
grep -q "HLS lista: seq" $B/h.log && [ $(grep -c "HLS lista: seq" $B/h.log) -ge 3 ] && ok "playlist reloaded as it advanced ($(grep -c 'HLS lista: seq' $B/h.log) times)" || bad "reloads"
for i in $(seq 1 100); do grep -q "^closed" $B/ph.log && break; sleep 0.1; done
mh=$(grep -o "minahead=[-0-9]*" $B/ph.log | cut -d= -f2)
[ -n "$mh" ] && [ $mh -ge 9375 ] && ok "HLS reader never caught up (min $mh bytes ahead)" || bad "hls minahead '$mh'"
if waitst $D/h/st stopped 8; then ok "stopped after the player closed it"; else bad "hls st=$(st $D/h/st st)"; fi
sleep 0.3; noaudio $D/h && ok "no audio file left" || bad "audio left behind"

echo "== 8. HLS packed AAC with ID3 tags, gzip playlist"
for v in packed gz; do
  HIBYRADIO_NOREADER=1 HIBYNET_VERBOSE=1 radio $U/hls/$v/index.m3u8 "$v" $D/$v 4 2> $B/$v.log & HP=$!; PIDS="$PIDS $HP"
  if waitst $D/$v/st ready 15; then ok "$v: ready"; else bad "$v: st=$(st $D/$v/st st) err=$(st $D/$v/st err)"; fi
  sleep 5
  cp "$(st $D/$v/st file)" $B/$v.aac 2>/dev/null || true
  r=$(python3 $H/tests/radio_frames_check.py $B/$v.aac)
  case "$r" in *ok=1*) ok "$v: $r";; *) bad "$v: $r";; esac
  kill -TERM $HP 2>/dev/null; sleep 0.3
  noaudio $D/$v || bad "$v: audio left behind"
done
grep -q "lista comprimida" $B/gz.log && ok "gzip playlist inflated" || bad "gzip not seen"

echo "== 9. session cap"
HIBYRADIO_PLAYER=rt_player HIBYRADIO_CAP_KB=400 HIBYNET_VERBOSE=1 radio $U/fast "Cap" $D/k 3 2> $B/k.log & HP=$!; PIDS="$PIDS $HP"
FAKE_EOFSTOP=1 $B/rt_player $D/k/st/state 64000 30 0 > $B/pk.log & RP=$!; PIDS="$PIDS $RP"
if waitst $D/k/st stopped 30; then ok "stopped"; else bad "cap: st=$(st $D/k/st st) err=$(st $D/k/st err)"; fi
[ "$(st $D/k/st err)" = cap ] && ok "err=cap (clear end, not an error)" || bad "cap err '$(st $D/k/st err)'"
t=$(grep -o "eof gen=1 pos=[0-9]*" $B/pk.log | head -1 | sed 's/.*pos=//')
[ -n "$t" ] && [ $t -le 409600 ] && [ $t -ge 380000 ] && ok "the file stopped at the cap ($t bytes)" || bad "cap size '$t'"
sleep 0.5; noaudio $D/k && ok "no audio file left" || bad "audio left behind"

echo "== 10. no SD card"
timeout 10 $B/hibynet radio $U/icy /usr/data/mnt/sd_9/HiByOS/cache/radio "X" 15 128 $D/nosd 2>/dev/null || true
[ "$(st $D/nosd st)" = error ] && [ "$(st $D/nosd err)" = nosd ] && ok "err=nosd" || bad "nosd: st=$(st $D/nosd st) err=$(st $D/nosd err)"
[ -e /usr/data/mnt/sd_9 ] && bad "created the mount point folder" || ok "nothing created on internal storage"

echo "== 11. the shell dies"
mkdir -p $D/z
bash -c "HIBYRADIO_NOREADER=1 $B/hibynet radio $U/icy $D/z Z 3 128 $D/z/st 2>/dev/null & wait" & SH=$!
waitst $D/z/st ready 12 || true
hp=$(cat $D/z/st/pid 2>/dev/null)
kill -9 $SH; sleep 1.5
[ -n "$hp" ] && ! kill -0 $hp 2>/dev/null && ok "helper exited with its shell" || { bad "helper $hp still running"; kill $hp 2>/dev/null; }
noaudio $D/z && ok "no audio file left" || bad "audio left behind: $(ls $D/z)"

echo "radio helper: $fails failures"
[ $fails = 0 ]
