#!/bin/bash
# Frames of the Radio app as PNG ($RADIO_OUT, default out/radio_render/), with the firmware font, to
# review the design without the device. The drawing code is the real one (src/radio_ui.inc.c and the
# shell's primitives copied as they are); network, threads and touch are stubs. Also checks that no
# touch zone leaves the screen or overlaps another one.
#   bash tests/radio_render.sh
set -e
cd "$(dirname "$0")/.."
S=src
F=$HOME/hiby/rootfs/usr/resource/fonts/
X=/tmp/radio_render_extract.inc
# drawing functions of the shell, copied as they are ("static ... name(" up to the "}" of column 0)
ext() { awk -v n="$2" '{sub(/\r$/, "")} !p && $0 ~ "^static[^(]*[ *]"n"\\(" && $0 !~ /\);[ \t]*$/ {p=1; print; if ($0 ~ /}[ \t]*$/) p=0; next} p {print} p && /^}/ {p=0}' "$1"; }
{
  awk '{sub(/\r$/, "")} /^#define AA_BLEND/ {p=1} p {print} p && !/\\$/ {p=0}' $S/launcher.inc.c
  ext $S/launcher.inc.c aa_span
  ext $S/launcher.inc.c fill_circle
  ext $S/launcher.inc.c fill_rrect
  grep -m1 '^static int wrap_cw;' $S/settings_app.inc.c
  ext $S/settings_app.inc.c wrap_cjk; ext $S/settings_app.inc.c wrap_nostart; ext $S/settings_app.inc.c wrap_emit; ext $S/settings_app.inc.c draw_wrapped
  ext $S/settings_app.inc.c sp_mix; ext $S/settings_app.inc.c sp_switch
  grep -m1 '^#define SPN ' $S/store.inc.c; grep -m1 '^static uint8_t spn_cov' $S/store.inc.c; grep -m1 '^static int spn_fade' $S/store.inc.c
  ext $S/store.inc.c spn_init; ext $S/store.inc.c spn_phase; ext $S/store.inc.c spn_draw
  grep '^#define FMC_' $S/freemusic.inc.c
  awk '{sub(/\r$/, "")} /^enum \{ FH_ROW = 1/ {p=1} p {print} p && /;$/ {p=0}' $S/freemusic.inc.c
  awk '{sub(/\r$/, "")} /^struct fm_hit \{/ {p=1} p {print} p && /^static int fm_press_kind/ {p=0}' $S/freemusic.inc.c
  for f in fm_hline fm_line fm_ringd fm_glyph_search fm_glyph_x fm_glyph_back fm_glyph_play fm_glyph_check fm_glyph_more fm_glyph_bars fm_badge \
           fm_hit fm_hit_at fm_pressed fm_chrome_status fm_navbar fm_spinner_at; do ext $S/freemusic.inc.c $f; done
} > $X
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-misleading-indentation -Wno-sign-compare -Wno-format-truncation \
  -fsanitize=address,undefined -DTEXT_FONT_DIRS="\"$F\"" -I$S -I$HOME/hiby/third -I/tmp -o /tmp/radio_render tests/radio_render.c -lm
mkdir -p ${RADIO_OUT:-out/radio_render}
/tmp/radio_render
