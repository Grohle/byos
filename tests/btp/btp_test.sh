#!/bin/bash
# PC tests (WSL, gcc) of hibysync and the BTP/1 code. Never touches the device.
#   wsl -d Ubuntu -- bash /mnt/c/Users/alber/Downloads/Hiby/hibymod/tests/btp/btp_test.sh
# 1. vectors: every entry of tests/btp/vectors.json against src/btp_*.inc.c (ASan + UBSan)
# 2. daemon: the real daemon (x86 test build, paths from HIBYSYNC_* variables) on a temporary
#    SD root, driven over TCP and a fake shell control pipe (ASan + UBSan)
# 3. the release core compiles without warnings (-Wall -Wextra, no test hooks)
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)
T=/tmp/btptest
rm -rf $T; mkdir -p $T
fails=0
CF="-O1 -g -Wall -Wextra -Wno-unused-function -Wno-sign-compare -Wno-unused-parameter -Wno-format-truncation -fsanitize=address,undefined -fno-sanitize-recover=undefined"

echo "=== build"
gcc $CF -DBTP_TEST -I$R/src -o $T/hibysync-x86 $R/src/hibysync.c || { echo "build of the daemon failed"; exit 1; }
gcc $CF -I$R/src -o $T/btp_test $R/tests/btp/btp_test.c || { echo "build of the test failed"; exit 1; }

echo "=== 1. vectors"
ASAN_OPTIONS=detect_leaks=0 $T/btp_test vectors $R/tests/btp/vectors.json || fails=$((fails + 1))

echo "=== 2. daemon"
ASAN_OPTIONS=detect_leaks=0 $T/btp_test daemon $T/hibysync-x86 $T/work || fails=$((fails + 1))
if grep -q "ERROR: AddressSanitizer\|runtime error" $T/work/daemon.log 2>/dev/null; then
    echo "  FAIL sanitizer report in the daemon log:"; grep -A5 "AddressSanitizer\|runtime error" $T/work/daemon.log | head -30; fails=$((fails + 1))
else echo "  ok   no sanitizer reports from the daemon"; fi

echo "=== 3. release build of the core (no BTP_TEST) compiles cleanly"
if gcc -fsyntax-only -Wall -Wextra -Wno-unused-function -I$R/src $R/src/hibysync.c 2>&1 | grep -q .; then
    gcc -fsyntax-only -Wall -Wextra -Wno-unused-function -I$R/src $R/src/hibysync.c; echo "  FAIL warnings"; fails=$((fails + 1))
else echo "  ok   no warnings"; fi

[ $fails = 0 ] && echo "ALL OK" || echo "$fails FAILED PARTS"
exit $fails
