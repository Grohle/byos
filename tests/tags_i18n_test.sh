#!/bin/bash
# Textos del editor de etiquetas en los 13 idiomas (tabla lang/tags_tr.inc.c y todas las claves
# que usa src/tageditor.inc.c: tr("...") y N_("...")). bash tests/tags_i18n_test.sh  (WSL; sin aparato)
set -e
H=/mnt/c/Users/alber/Downloads/Hiby/hibymod
F=$HOME/hiby/rootfs/usr/resource/fonts/
grep -oE '\b(tr|N_)\("([^"\\]|\\.)*"\)' $H/src/tageditor.inc.c | sed -E 's/^(tr|N_)\("//;s/"\)$//' | sort -u > /tmp/tags_keys.txt
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-sign-compare -Wno-unused-parameter -fsanitize=address,undefined \
  -DTEXT_FONT_DIRS="\"$F\"" -I$H/src -I$HOME/hiby/third -o /tmp/tags_i18n_test $H/tests/tags_i18n_test.c -lm
/tmp/tags_i18n_test /tmp/tags_keys.txt
