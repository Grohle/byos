#!/bin/bash
# Fotogramas del editor de etiquetas en PNG ($TAGS_OUT, por defecto out/tags_render/), con las fuentes del firmware.
#   bash tests/tags_render.sh
set -e
cd "$(dirname "$0")/.."
S=src
F=$HOME/hiby/rootfs/usr/resource/fonts/
X=/tmp/tags_render_extract.inc
# funciones de dibujo del shell, copiadas tal cual (desde "static ... nombre(" hasta la "}" de la columna 0)
ext() { awk -v n="$2" '{sub(/\r$/, "")} !p && $0 ~ "^static[^(]*[ *]"n"\\(" && $0 !~ /\);[ \t]*$/ {p=1; print; if ($0 ~ /}[ \t]*$/) p=0; next} p {print} p && /^}/ {p=0}' "$1"; }
{
  ext $S/launcher.inc.c fill_rrect
  grep -m1 '^static int wrap_cw;' $S/settings_app.inc.c
  ext $S/settings_app.inc.c wrap_cjk; ext $S/settings_app.inc.c wrap_nostart; ext $S/settings_app.inc.c wrap_emit; ext $S/settings_app.inc.c draw_wrapped
} > $X
# el programa hibytag de verdad (ASSETS=/tmp en el arnes): dbupdate/dbrename contra una base de prueba
T=$HOME/hiby/third
[ -f /tmp/sqlite3_x86.o ] || gcc -O1 -w -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -c $T/sqlite3.c -o /tmp/sqlite3_x86.o
gcc -O1 -w -DHIBYTAG_MAIN -I$T -o /tmp/hibytag $S/hibytag.c /tmp/sqlite3_x86.o
gcc -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-misleading-indentation -Wno-unused-result -Wno-format-truncation -fsanitize=address,undefined \
  -DTEXT_FONT_DIRS="\"$F\"" -I$S -I$T -I/tmp -o /tmp/tags_render tests/tags_render.c /tmp/sqlite3_x86.o -lm
/tmp/tags_render
