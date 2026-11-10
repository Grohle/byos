#!/bin/bash
# Frames of Settings > General > Software Update as PNG ($OTA_OUT, default out/ota_render/), with
# the firmware font and the shell's own page renderer. Also fails if a title or value would be
# cut with "…". Does not touch the device.
#   wsl -d Ubuntu -- bash /mnt/c/Users/alber/Downloads/Hiby/hibymod/tests/ota_render.sh
set -e
cd "$(dirname "$0")/.."
S=src
F=$HOME/hiby/rootfs/usr/resource/fonts/
X=/tmp/ota_render_x; mkdir -p $X
# drawing functions of the shell, copied as they are ("static ... name(" up to the "}" of column 0)
ext() { awk -v n="$2" '{sub(/\r$/, "")} !p && $0 ~ "^static[^(]*[ *]"n"\\(" && $0 !~ /\);[ \t]*$/ {p=1; print; if ($0 ~ /}[ \t]*$/) p=0; next} p {print} p && /^}/ {p=0}' "$1"; }
{
  awk '{sub(/\r$/, "")} /^#define AA_BLEND/ {p=1} p {print} p && !/\\$/ {p=0}' $S/launcher.inc.c
  ext $S/launcher.inc.c aa_span
  ext $S/launcher.inc.c fill_circle
  ext $S/launcher.inc.c fill_rrect
  # settings_app.inc.c from the top to the end of sp_render (types, wrapping, switches, rows, page)
  awk '{sub(/\r$/, "")} /^\/\* fila en \(x,y\) o -1 \*\// {exit} {print}' $S/settings_app.inc.c
} > $X/ota_render_extract.inc
gcc -O2 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-misleading-indentation -Wno-unused-result -Wno-format-truncation \
  -DTEXT_FONT_DIRS="\"$F\"" -I$S -I$HOME/hiby/third -I$X -o $X/ota_render tests/ota_render.c -lm -lpthread
OUT=${OTA_OUT:-out/ota_render}; rm -rf $OUT; mkdir -p $OUT
OTA_OUT=$OUT $X/ota_render
