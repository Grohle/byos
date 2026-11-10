#!/bin/bash
# hibymix: estatico (musl) con SQLite (sin extensiones, sin hilos)
set -e
. "$(dirname "$0")/fetch.sh"; zig_check   # pinned compiler: ziglang 0.17.1, see tools/wsl/fetch.sh
S=/mnt/c/Users/alber/Downloads/Hiby/hibymod/src
~/hiby/venv/bin/python -m ziglang cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os -static -s \
  -I$HOME/hiby/third -w \
  -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED \
  -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_DEFAULT_CACHE_SIZE=-256 \
  -o /mnt/c/Users/alber/Downloads/Hiby/hibymod/bin/hibymix $S/hibymix.c $HOME/hiby/third/sqlite3.c
ls -la /mnt/c/Users/alber/Downloads/Hiby/hibymod/bin/hibymix
