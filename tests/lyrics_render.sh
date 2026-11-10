#!/bin/bash
# Fotogramas de la busqueda de letras en PNG ($LYR_OUT, por defecto out/lyrics_render/), con las fuentes del firmware.
#   bash tests/lyrics_render.sh
set -e
cd "$(dirname "$0")/.."
S=src
F=$HOME/hiby/rootfs/usr/resource/fonts/
X=/tmp/lyrics_render_extract.inc
# funciones de dibujo del shell, copiadas tal cual (desde "static ... nombre(" hasta la "}" de la columna 0)
ext() { awk -v n="$2" '{sub(/\r$/, "")} !p && $0 ~ "^static[^(]*[ *]"n"\\(" && $0 !~ /\);[ \t]*$/ {p=1; print; if ($0 ~ /}[ \t]*$/) p=0; next} p {print} p && /^}/ {p=0}' "$1"; }
{
  ext $S/launcher.inc.c fill_circle; ext $S/launcher.inc.c fill_rrect
  ext $S/music.inc.c mu_text_bold; ext $S/music.inc.c mu_line
  grep -m1 '^static int wrap_cw;' $S/settings_app.inc.c
  ext $S/settings_app.inc.c wrap_cjk; ext $S/settings_app.inc.c wrap_nostart; ext $S/settings_app.inc.c wrap_emit; ext $S/settings_app.inc.c draw_wrapped
} > $X
gcc -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-misleading-indentation -fsanitize=address,undefined \
  -DTEXT_FONT_DIRS="\"$F\"" -I$S -I$HOME/hiby/third -I/tmp -o /tmp/lyrics_render tests/lyrics_render.c -lm
/tmp/lyrics_render
