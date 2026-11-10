#!/bin/bash
set -e
cd ~/hiby
M=/mnt/c/Users/alber/Downloads/Hiby/hibymod
mkdir -p $M/out
for f in $(find $M/overlay -type f); do head -c2 "$f" | grep -q "#!" && sh -n "$f" && echo "sintaxis OK: ${f#$M/}"; done
unshare -r venv/bin/python -I $M/tools/build.py --stock orig/r3ii_2025.upt --overlay $M/overlay --out $M/out/r3ii_2025.upt
md5sum $M/out/r3ii_2025.upt
