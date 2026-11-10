#!/bin/bash
# hibywall: estatico (musl, soft-float) con libjpeg 9f (solo decodificador) y stb_image
set -e
J=~/hiby/third/jpeg-9f
cat > $J/jconfig.h <<'EOF'
#define HAVE_PROTOTYPES
#define HAVE_UNSIGNED_CHAR
#define HAVE_UNSIGNED_SHORT
#define HAVE_STDDEF_H
#define HAVE_STDLIB_H
#undef NEED_BSD_STRINGS
#undef NEED_SYS_TYPES_H
#undef NEED_FAR_POINTERS
#undef NEED_SHORT_EXTERNAL_NAMES
#undef INCOMPLETE_TYPES_BROKEN
EOF
SRC="jaricom jcomapi jdapimin jdapistd jdarith jdatasrc jdcoefct jdcolor jddctmgr jdhuff jdinput jdmainct jdmarker jdmaster jdmerge jdpostct jdsample jerror jidctflt jidctfst jidctint jmemmgr jmemnobs jquant1 jquant2 jutils"
files=""
for s in $SRC; do files="$files $J/$s.c"; done
~/hiby/venv/bin/python -m ziglang cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os -static -s \
  -I$J -I$HOME/hiby/third -w -o "$2" "$1" $files
file "$2"; ls -la "$2"
