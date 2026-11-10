#!/bin/bash
set -e
Z="$HOME/hiby/venv/bin/python -m ziglang cc -target mipsel-linux-musleabi -mcpu=mips32r2 -msoft-float -Os -static -s"
mkdir -p ~/hiby/vm/out/bin
for t in hibyinput evdump memhog fbtouch; do
  $Z -o ~/hiby/vm/out/bin/$t ~/hiby/vm/tools/$t.c
done
file ~/hiby/vm/out/bin/*; ls -la ~/hiby/vm/out/bin
