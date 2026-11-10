#!/bin/bash
# PC tests of the Free Music parsers (src/freemusic_core.inc.c) against recorded responses.
# bash tests/freemusic_test.sh            (WSL; no device)
set -e
H=/mnt/c/Users/alber/Downloads/Hiby/hibymod
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-sign-compare -fsanitize=address,undefined \
  -I$H/src -o /tmp/freemusic_test $H/tests/freemusic_test.c
cd $H && /tmp/freemusic_test tests/freemusic_samples
# texts in 13 languages: the table and every key used by the app (tr("...") and the indirect ones)
O=$H/out/agent_freemusic/src
I=$H/src; grep -q freemusic_tr $I/i18n.inc.c || I=$O      # before the hook is in src/i18n.inc.c
F=$HOME/hiby/rootfs/usr/resource/fonts/
{ grep -o 'tr("[^"]*")' $H/src/freemusic.inc.c | sed 's/^tr("//;s/")$//'
  python3 - $H/src/freemusic.inc.c $H/src/freemusic_core.inc.c <<'PY'
import re,sys
a=open(sys.argv[1],encoding='utf-8').read(); c=open(sys.argv[2],encoding='utf-8').read()
for name in ('sec_ia','sec_jm','ts','n'):
    for m in re.finditer(r'const char \*const '+name+r'\[[^\]]*\] = \{([^}]*)\}', a):
        for k in re.findall(r'"([^"]*)"', m.group(1)):
            if k: print(k)
for m in re.finditer(r'\{ "([^"]+)", "[^"]*", "[^"]*" \}', c[c.index('FM_GENRES[]'):c.index('FM_NGENRE')]): print(m.group(1))
PY
} | sort -u > /tmp/freemusic_keys.txt
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-sign-compare -Wno-unused-parameter -fsanitize=address,undefined \
  -DTEXT_FONT_DIRS="\"$F\"" -I$I -I$HOME/hiby/third -o /tmp/freemusic_i18n_test $H/tests/freemusic_i18n_test.c -lm
/tmp/freemusic_i18n_test /tmp/freemusic_keys.txt
