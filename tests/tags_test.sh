#!/bin/bash
# Pruebas en PC de hibytag: todas las etiquetas, renombrar y rutas de la base (no toca el aparato)
#   bash tests/tags_test.sh
set -e
H=/mnt/c/Users/alber/Downloads/Hiby/hibymod
T=$HOME/hiby/third
[ -f /tmp/sqlite3_x86.o ] || gcc -O1 -w -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -c $T/sqlite3.c -o /tmp/sqlite3_x86.o
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-sign-compare -Wno-format-truncation -fsanitize=address,undefined \
  -I$H/src -I$T -o /tmp/tags_test $H/tests/tags_test.c /tmp/sqlite3_x86.o -lm
/tmp/tags_test /tmp/hibytag_tags_test
# CLI: getall/setkv sobre una muestra y la ayuda
gcc -O1 -w -DHIBYTAG_MAIN -I$T -o /tmp/hibytag_cli $H/src/hibytag.c /tmp/sqlite3_x86.o
HIBYTAG_NODB=1 /tmp/hibytag_cli setkv /tmp/hibytag_tags_test/a.flac COMPOSER=CLI "COMMENT=uno dos" > /dev/null
/tmp/hibytag_cli getall /tmp/hibytag_tags_test/a.flac | grep -q '^COMPOSER=CLI$'
/tmp/hibytag_cli getall /tmp/hibytag_tags_test/a.flac | grep -q '^COMMENT=uno dos$'
echo "CLI ok"
# comprobacion cruzada con una libreria independiente (mutagen; se omite si no esta).
# MUTAGEN_PY=/ruta/a/python con mutagen (p. ej. python3 -m venv /tmp/mv && /tmp/mv/bin/pip install mutagen)
PY=${MUTAGEN_PY:-python3}; [ -x /tmp/mv/bin/python ] && [ -z "$MUTAGEN_PY" ] && PY=/tmp/mv/bin/python
$PY $H/tests/tags_mutagen_check.py /tmp/hibytag_tags_test
