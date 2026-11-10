#!/bin/bash
# hibyshell: estatico (musl, soft-float), con stb_truetype
set -e
S=/mnt/c/Users/alber/Downloads/Hiby/hibymod/src
~/hiby/venv/bin/python -m ziglang cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -O2 -static -s \
  -I$S -I$HOME/hiby/third -Wall -Wno-unused-function -Wno-misleading-indentation -Wno-unused-result \
  -o /mnt/c/Users/alber/Downloads/Hiby/hibymod/bin/hibyshell $S/hibyshell.c -lm
ls -la /mnt/c/Users/alber/Downloads/Hiby/hibymod/bin/hibyshell
