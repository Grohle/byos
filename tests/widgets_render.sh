#!/bin/bash
# Home Screen widgets as PNG ($WG_OUT, default out/widgets_render/), every type and size in the
# 13 languages, on the default wallpaper; fails if an interface text is cut. Does not touch the device.
#   bash tests/widgets_render.sh
set -e
cd "$(dirname "$0")/.."
S=src
F=$HOME/hiby/rootfs/usr/resource/fonts/
D=/tmp/wg_render; mkdir -p $D
X=$D/wg_render_extract.inc
ext() { awk -v n="$2" '{sub(/\r$/, "")} !p && $0 ~ "^static[^(]*[ *]"n"[(]" && $0 !~ /\);[ \t]*$/ {p=1; print; if ($0 ~ /}[ \t]*$/) p=0; next} p {print} p && /^}/ {p=0}' "$1"; }
{
  awk '{sub(/\r$/, "")} /^#define AA_BLEND/ {p=1} p {print} p && !/\\$/ {p=0}' $S/launcher.inc.c
  ext $S/launcher.inc.c aa_span
  ext $S/launcher.inc.c fill_circle
  ext $S/launcher.inc.c fill_rrect
  grep -m1 '^static int wrap_cw;' $S/settings_app.inc.c
  ext $S/settings_app.inc.c wrap_cjk; ext $S/settings_app.inc.c wrap_nostart; ext $S/settings_app.inc.c wrap_emit; ext $S/settings_app.inc.c draw_wrapped
  ext $S/music.inc.c mu_scale
} > $X
gcc -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-misleading-indentation -Wno-unused-result -Wno-format-truncation \
  -o $D/hibybudsd_x86 $S/hibybudsd.c
gcc -O2 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-misleading-indentation -Wno-unused-result -Wno-format-truncation \
  -DTEXT_FONT_DIRS="\"$F\"" -I$S -I$HOME/hiby/third -I$D -o $D/widgets_render tests/widgets_render.c -lm -lpthread
pkill -f "hibybudsd_x86 --fake" 2>/dev/null || true
rm -f /tmp/hibybuds.state /tmp/hibybuds.cmd /tmp/hibybuds.evt /tmp/hibybudsd.pid
HIBYBUDS_DIR=/tmp HIBYBUDS_DATA=$D/data $D/hibybudsd_x86 --fake & P=$!
trap 'kill $P 2>/dev/null' EXIT
sleep 1
mkdir -p ${WG_OUT:-out/widgets_render}
$D/widgets_render
