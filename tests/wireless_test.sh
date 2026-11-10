#!/bin/bash
# pruebas en PC de la app Inalambrico: analizadores (wpa_supplicant, bluez) y textos en 13 idiomas.
# No toca el aparato.
set -e
H=/mnt/c/Users/alber/Downloads/Hiby/hibymod
F=$HOME/hiby/rootfs/usr/resource/fonts/
gcc -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -I$H/src \
  -o /tmp/wireless_test $H/tests/wireless_test.c
/tmp/wireless_test
# claves de tr() y de los avisos de los hilos en wireless.inc.c (y cabeceras de seccion)
{ grep -o 'tr("[^"]*")' $H/src/wireless.inc.c | sed 's/^tr("//;s/")$//'
  grep -o 'wl_msg("[^"]*"' $H/src/wireless.inc.c | sed 's/^wl_msg("//;s/"$//'
  grep -o 'wl_msg(j.iv ? "[^"]*" : "[^"]*"' $H/src/wireless.inc.c | sed 's/.*? "//;s/" : "/\n/;s/"$//'
  grep -o 'wp_sec(pg, "[^"]*"' $H/src/wireless.inc.c | sed 's/^wp_sec(pg, "//;s/"$//'
  grep -o 'wp_foot(pg, tr("[^"]*")' $H/src/wireless.inc.c | sed 's/^wp_foot(pg, tr("//;s/")$//'
} | sort -u > /tmp/wireless_keys.txt
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-sign-compare -Wno-unused-parameter -fsanitize=address,undefined \
  -DTEXT_FONT_DIRS="\"$F\"" -I$H/src -I$HOME/hiby/third -o /tmp/wireless_i18n_test $H/tests/wireless_i18n_test.c -lm
/tmp/wireless_i18n_test /tmp/wireless_keys.txt
