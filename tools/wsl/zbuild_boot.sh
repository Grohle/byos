set -e
. "$(dirname "$0")/fetch.sh"; zig_check   # pinned compiler: ziglang 0.17.1, see tools/wsl/fetch.sh
S=/mnt/c/Users/alber/Downloads/Hiby/hibymod/src
~/hiby/venv/bin/python -m ziglang cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -O2 -static -s -I$HOME/hiby/third -Wall -o /mnt/c/Users/alber/Downloads/Hiby/hibymod/bin/hibyboot $S/hibyboot.c
ls -la /mnt/c/Users/alber/Downloads/Hiby/hibymod/bin/hibyboot
