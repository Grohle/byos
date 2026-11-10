#!/bin/bash
# PC tests of the headphone management (docs/HEADPHONES.md). Does not touch the device.
#   1. protocols (tests/buds_test.c, ASan/UBSan): Apple, Samsung, Nothing, Sony, Fast Pair, SDP, state
#   2. the fake daemon end to end (real brand modules against the simulator), through the FIFO
#   3. docs/HEADPHONES.md matrix == the capability tables (hibybudsd --matrix)
#   4. every tr() key of the app in the 13 languages (keys, %s, glyphs of the firmware font)
set -e
H=/mnt/c/Users/alber/Downloads/Hiby/hibymod
F=$HOME/hiby/rootfs/usr/resource/fonts/
gcc -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -o /tmp/buds_test $H/tests/buds_test.c
/tmp/buds_test
gcc -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -o /tmp/hibybudsd_x86 $H/src/hibybudsd.c
# fake daemon: AirPods Pro 2 connected; connect the Galaxy Buds2 Pro; commands to both
D=$(mktemp -d); export HIBYBUDS_DIR=$D HIBYBUDS_DATA=$D/data
/tmp/hibybudsd_x86 --fake & P=$!
trap 'kill $P 2>/dev/null; rm -rf $D' EXIT
sleep 1
ok() { if grep -q "$1" <(sed -n "/dev=$2/,/end/p" $D/hibybuds.state); then echo "  ok: $3"; else echo "FAIL: $3"; sed -n "/dev=$2/,/end/p" $D/hibybuds.state; exit 1; fi; }
A=F0:5C:77:A1:02:14; S=F0:5C:77:5A:07:02
ok '^conn=3' $A "AirPods Pro 2 ready"
ok '^popseq=1' $A "popup event"
echo "1 $A noise 2 0 0" > $D/hibybuds.cmd; echo "2 $S connect 0 0 0" > $D/hibybuds.cmd; sleep 2.5
ok '^noise=2,15' $A "AirPods transparency"
ok '^conn=3' $S "Buds2 Pro connected (fake)"
ok '^conn=0' $A "AirPods dropped (A2DP to one device)"
echo "3 $S noise 1 0 0" > $D/hibybuds.cmd; echo "4 $S eq 3 0 0" > $D/hibybuds.cmd; echo "5 $S gest 0 3 8" > $D/hibybuds.cmd; sleep 0.6
ok '^noise=1,7' $S "Galaxy ANC"; ok '^eq=3,6' $S "Galaxy EQ"; ok '^gestl=-1,-1,-1,8' $S "Galaxy touch and hold = volume"
echo "6 $S noise 3 0 0" > $D/hibybuds.cmd; sleep 0.4
grep -q '^err=6,-4' $D/hibybuds.state && echo "  ok: adaptive refused on Buds2 Pro" || { echo "FAIL: range error"; exit 1; }
kill $P; wait $P 2>/dev/null || true
# the doc's matrix is generated from the tables
/tmp/hibybudsd_x86 --matrix > /tmp/buds_matrix.md
sed -n '/<!-- matrix:begin/,/<!-- matrix:end -->/p' $H/docs/HEADPHONES.md | tr -d '\r' > /tmp/buds_matrix_doc.md
if cmp -s /tmp/buds_matrix.md /tmp/buds_matrix_doc.md; then echo "  ok: docs/HEADPHONES.md matrix up to date"; else echo "FAIL: docs/HEADPHONES.md matrix differs from the tables (regenerate with hibybudsd --matrix)"; diff /tmp/buds_matrix.md /tmp/buds_matrix_doc.md | head; exit 1; fi
# translations
{ grep -o 'tr("[^"]*")' $H/src/buds.inc.c | sed 's/^tr("//;s/")$//'
  grep -o 'bu_sec(pg, "[^"]*"' $H/src/buds.inc.c | sed 's/^bu_sec(pg, "//;s/"$//'
  grep -o '? "[A-ZÁÉÍÓÚ ]*" : "[A-ZÁÉÍÓÚ ]*"' $H/src/buds.inc.c | sed 's/? "//;s/" : "/\n/;s/"$//'
  for v in BU_ACTIONS BU_GESTURES; do grep "$v\[" $H/src/buds.inc.c | head -2 | grep -o '"[^"]*"' | tr -d '"'; done
  grep -E '^static const char \*const BUDS_EQ_' $H/src/buds_core.inc.c | grep -o '"[^"]*"' | tr -d '"'
  grep -o 'lb\[3\] = { "[^}]*' $H/src/buds.inc.c | grep -o '"[^"]*"' | tr -d '"'
  grep -o 'bn\[3\] = { "[^}]*' $H/src/buds.inc.c | grep -o '"[^"]*"' | tr -d '"'
} | grep -v '^Spotify$\|^Siri$' | sort -u > /tmp/buds_keys.txt
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-sign-compare -Wno-unused-parameter -fsanitize=address,undefined \
  -DTEXT_FONT_DIRS="\"$F\"" -I$H/src -I$HOME/hiby/third -o /tmp/buds_i18n_test $H/tests/buds_i18n_test.c -lm
/tmp/buds_i18n_test /tmp/buds_keys.txt
