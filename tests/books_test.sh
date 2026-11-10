#!/bin/bash
# pruebas en PC del nucleo de Libros (EPUB/TXT, inflate, maquetacion); no toca el aparato
set -e
H=/mnt/c/Users/alber/Downloads/Hiby/hibymod
R=$HOME/hiby/rootfs/usr/resource
S=/tmp/books_samples
python3 -I $H/tests/books_samples.py $S >/dev/null
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-sign-compare -Wno-unused-parameter -Wno-unused-result -fsanitize=address,undefined \
  -DTEXT_FONT_DIRS="\"$R/fonts/\"" -DBK_GBK_TABLE="\"$R/unicode/gb18030_unicode_tbl.bin\"" \
  -I$H/src -I$HOME/hiby/third -o /tmp/books_test $H/tests/books_test.c -lm
/tmp/books_test $S "$@"
