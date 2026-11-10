#!/bin/bash
# Pruebas del lector .lrc y del orden de resultados de letras (src/lrc_core.inc.c), en el PC.
#   bash tests/lyrics_test.sh
set -e
cd "$(dirname "$0")/.."
gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o /tmp/lyrics_test tests/lyrics_test.c
/tmp/lyrics_test
