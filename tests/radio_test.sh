#!/bin/bash
# PC tests of the Radio app (docs/RADIO.md). Does not touch the device.
#   1. pure parts (tests/radio_test.c, ASan/UBSan): Radio Browser JSON, station library TSV,
#      helper state and settings, .m3u/.pls, ICY metadata, chunked bodies, MP3/ADTS frame sync,
#      HLS playlists, MPEG-TS demux (a real RNE segment), station sources and the European list
#   2. every tr() key of the app (and the genre names) in the 13 languages (keys, %d, glyphs)
#   3. the stream helper (hibynet radio) end to end: tests/radio_helper_test.sh
#   4. frames of the app in 7 languages (tests/radio_render.sh, out/radio_render) + touch zone checks
# bash tests/radio_test.sh            (WSL; no device)
set -e
H=${H:-$(cd "$(dirname "$0")/.." && pwd)}      # this checkout (or a worktree of it)
F=$HOME/hiby/rootfs/usr/resource/fonts/
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-sign-compare -fsanitize=address,undefined \
  -I$H/src -o /tmp/radio_test $H/tests/radio_test.c
cd $H && /tmp/radio_test tests/radio_samples
{ grep -oh 'tr("[^"]*")' $H/src/radio.inc.c $H/src/radio_ui.inc.c $H/src/radio_app.inc.c | sed 's/^tr("//;s/")$//'
  sed -n '/RA_GENRES\[\] = {/,/^};/p' $H/src/radio_core.inc.c | grep -o '{ "[^"]*", "[^"]*" }' | sed 's/^{ "//;s/", ".*//'
} | sort -u > /tmp/radio_keys.txt
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-sign-compare -Wno-unused-parameter -fsanitize=address,undefined \
  -DTEXT_FONT_DIRS="\"$F\"" -I$H/src -I$HOME/hiby/third -o /tmp/radio_i18n_test $H/tests/radio_i18n_test.c -lm
/tmp/radio_i18n_test /tmp/radio_keys.txt
# 3. the stream helper end to end (local fake station + fake player; needs the x86 deps of zbuild_net.sh)
if [ -f $HOME/hiby/net/x86/lib/libmbedtls.a ]; then bash $H/tests/radio_helper_test.sh
else echo "skip: stream helper test (build the x86 deps: tools/wsl/zbuild_net.sh x86)"; fi
# 4. frames of the app as PNG (out/radio_render) + touch zones inside the screen and not overlapping
bash $H/tests/radio_render.sh
