#!/bin/bash
# Frames of the on-screen keyboard as PNG ($KB_OUT, default out/kb_render/): the layouts of the 13
# languages, popups, Hangul, Thai, the candidate bar, the globe menu and the keyboard sheet, with the
# firmware fonts. The same run drives the real kb_touch() with scripted touches and checks the result.
# Does not touch the device.
#   bash tests/kb_render.sh
set -e
cd "$(dirname "$0")/.."
S=src
F=$HOME/hiby/rootfs/usr/resource/fonts/
X=/tmp/kb_render_extract.inc
# drawing functions of the shell, copied as they are ("static ... name(" up to the "}" of column 0)
ext() { awk -v n="$2" '{sub(/\r$/, "")} !p && $0 ~ "^static[^(]*[ *]"n"\\(" && $0 !~ /\);[ \t]*$/ {p=1; print; if ($0 ~ /}[ \t]*$/) p=0; next} p {print} p && /^}/ {p=0}' "$1"; }
{
  awk '{sub(/\r$/, "")} /^#define AA_BLEND/ {p=1} p {print} p && !/\\$/ {p=0}' $S/launcher.inc.c
  ext $S/launcher.inc.c aa_span
  ext $S/launcher.inc.c fill_circle
  ext $S/launcher.inc.c fill_rrect
} > $X
gcc -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-misleading-indentation -Wno-unused-result -Wno-format-truncation \
  -fsanitize=address,undefined -DTEXT_FONT_DIRS="\"$F\"" -I$S -I$HOME/hiby/third -I/tmp -o /tmp/kb_render tests/kb_render.c -lm
mkdir -p ${KB_OUT:-out/kb_render}
/tmp/kb_render
ls ${KB_OUT:-out/kb_render} | wc -l
