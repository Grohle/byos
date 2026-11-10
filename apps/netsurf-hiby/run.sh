#!/bin/sh
# Lanza NetSurf en el HiBy (o en el arnes): fb0 + touch + OSK
D=$(cd "$(dirname "$0")" && pwd)
export HOME=${HOME:-/tmp}; mkdir -p $HOME/.netsurf; [ -f $HOME/.netsurf/Choices ] || cp $D/Choices $HOME/.netsurf/Choices
export NETSURFRES=$D/res/
exec $D/nsfb -f linux -w 320 -h 480 -b 16 "${1:-about:blank}"
