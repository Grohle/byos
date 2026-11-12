#!/bin/bash
# Host tests of the keyboard input engine (layouts of the 13 languages, Hangul, romaji, pinyin, Thai)
# with the firmware fonts. Does not touch the device.
#   bash tests/kb_test.sh
set -e
cd "$(dirname "$0")/.."
F=$HOME/hiby/rootfs/usr/resource/fonts/
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-sign-compare -Wno-unused-parameter -fsanitize=address,undefined \
  -DTEXT_FONT_DIRS="\"$F\"" -Isrc -I$HOME/hiby/third -o /tmp/kb_test tests/kb_test.c -lm
/tmp/kb_test "$@"
