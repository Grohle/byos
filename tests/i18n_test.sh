#!/bin/bash
# pruebas en PC de los 13 idiomas (claves, formatos, glifos, anchos); no toca el aparato
set -e
H=/mnt/c/Users/alber/Downloads/Hiby/hibymod
F=$HOME/hiby/rootfs/usr/resource/fonts/
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-sign-compare -Wno-unused-parameter -fsanitize=address,undefined \
  -DTEXT_FONT_DIRS="\"$F\"" -I$H/src -I$HOME/hiby/third -o /tmp/i18n_test $H/tests/i18n_test.c -lm
/tmp/i18n_test "$@"
