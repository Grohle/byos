#!/bin/bash
# hibysync: the ByOS sync daemon (BTP/1, docs/SYNC_PROTOCOL.md). Static (musl), small (-Os),
# no third-party libraries: SHA-256/Ed25519/JSON come from src/ota_net.inc.c (OTA_CORE_ONLY),
# X25519/XSalsa20-Poly1305/HKDF from src/btp_crypto.inc.c.
# Output: bin/hibysync (mipsel); with "x86" also ~/hiby/sync/hibysync-x86 (release paths, for a PC smoke test).
# Usage: tools/wsl/zbuild_sync.sh [x86]
# Tests: tests/btp/btp_test.sh (gcc, test build with overridable paths).
set -e
. "$(dirname "$0")/fetch.sh"
zig_check   # pinned compiler: ziglang 0.17.1, see tools/wsl/fetch.sh
R=/mnt/c/Users/alber/Downloads/Hiby/hibymod
S=$R/src
ZIG="$HOME/hiby/venv/bin/python -m ziglang"
CF="-Os -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-function -Wno-format-truncation -I$S"
$ZIG cc -target mipsel-linux-musleabihf -mcpu=mips32r2 $CF -static -s -Wl,--gc-sections -o $R/bin/hibysync $S/hibysync.c
ls -la $R/bin/hibysync
file $R/bin/hibysync
if [ "$1" = "x86" ]; then
  mkdir -p $HOME/hiby/sync
  gcc $CF -s -Wl,--gc-sections -o $HOME/hiby/sync/hibysync-x86 $S/hibysync.c
  ls -la $HOME/hiby/sync/hibysync-x86
fi
