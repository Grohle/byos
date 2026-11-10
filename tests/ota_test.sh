#!/bin/bash
# PC tests (WSL) of the online updates (docs/OTA.md). Never touches the device.
#   wsl -d Ubuntu -- bash /mnt/c/Users/alber/Downloads/Hiby/hibymod/tests/ota_test.sh
# 1. unit: src/ota_net.inc.c (SHA-256/512, Ed25519 against RFC 8032 + Python backends,
#    signatures good/bad/tampered/rotated, JSON, versions, hostile tars, manifests) and the
#    13-language table, with ASan/UBSan.
# 2. tools: ota_sign.py (genkey refuses the repo, sign/verify, pure-Python backend agrees),
#    ota_release.py (tar, signed JSON, key check).
# 3. flow: the real worker thread + hibynet (x86) + a local HTTP server with GitHub-like
#    redirects and Range, + the real hibyos_install.sh under dash (like tools/simtest_install.sh):
#    check, dropped and paused downloads resumed with Range, install, shell-vs-reboot, rollback
#    both ways, tampered tar/JSON, wrong key, downgrade, battery, no SD, failed install
#    restores the previous version, sys/ overrides of the installer, user state untouched.
set -u
R=$(cd "$(dirname "$0")/.." && pwd)
T=/tmp/otatest
FIX=$T/fix
NET=${OTA_TEST_NET:-$HOME/hiby/net/hibynet-x86-ota}
PY=python3
fails=0
ok()  { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; fails=$((fails + 1)); }
chk() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

rm -rf $T; mkdir -p $T
echo "=== fixtures"
$PY $R/tests/ota_fixtures.py $FIX || { echo "fixtures failed"; exit 1; }
PUB=$(cat $FIX/pub1.hex)

echo; echo "=== 1. unit (ASan + UBSan)"
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-sign-compare -Wno-unused-parameter -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -DOTA_PUBKEY_HEX="\"$PUB\"" -I$R/src -o $T/ota_test $R/tests/ota_test.c -lpthread \
    || { echo "build failed"; exit 1; }
$T/ota_test unit $FIX || fails=$((fails + 1))
# the shell's own build flags on the core (no warnings with -Wall -Wextra)
gcc -fsyntax-only -Wall -Wextra -Wno-unused-function -DOTA_CORE_ONLY -I$R/src -x c - <<'EOF' 2>&1 | grep -v '^$' && bad "warnings in the core" || ok "core compiles without warnings (-Wall -Wextra)"
static void lg(const char *f, ...) { (void)f; }
#include "ota_net.inc.c"
int main(void) { return 0; }
EOF

echo; echo "=== 2. tools"
KEYDIR=$T/keys; mkdir -p $KEYDIR
out=$($PY $R/tools/ota_sign.py genkey --key $R/tests/should_not_exist.key 2>&1); rc=$?
chk "genkey refuses a path inside the repository" "[ $rc != 0 ] && [ ! -f $R/tests/should_not_exist.key ]"
$PY $R/tools/ota_sign.py genkey --key $KEYDIR/k.key >/dev/null; rc=$?
chk "genkey outside the repo (mode 600)" "[ $rc = 0 ] && [ \"\$(stat -c %a $KEYDIR/k.key)\" = 600 ]"
$PY $R/tools/ota_sign.py genkey --key $KEYDIR/k.key >/dev/null 2>&1
chk "genkey never overwrites a key" "[ $? != 0 ]"
cp $FIX/json/good.json $T/s.json
sed -i 's/"signature": "ed25519:[0-9a-f]*"/"signature": "ed25519:'$(printf '0%.0s' $(seq 128))'"/' $T/s.json
$PY $R/tools/ota_sign.py sign --key $KEYDIR/k.key $T/s.json >/dev/null
KPUB=$($PY $R/tools/ota_sign.py pubkey --key $KEYDIR/k.key)
chk "sign + verify (cryptography)" "$PY $R/tools/ota_sign.py verify $T/s.json --pub $KPUB >/dev/null"
chk "verify with the pure-Python backend" "HIBYOS_OTA_BACKEND=pure $PY $R/tools/ota_sign.py verify $T/s.json --pub $KPUB >/dev/null"
chk "verify refuses another key" "! $PY $R/tools/ota_sign.py verify $T/s.json --pub $PUB >/dev/null"
sed -i 's/Fixed things/Fixed thingz/' $T/s.json
chk "verify refuses a tampered file" "! $PY $R/tools/ota_sign.py verify $T/s.json --pub $KPUB >/dev/null"
cp $R/src/ota_net.inc.c $T/src_backup.c
cp $R/src/ota_net.inc.c $T/ota_net_copy.c
$PY - "$T/ota_net_copy.c" "$KPUB" "$R/tools" <<'EOF'
import sys
sys.path.insert(0, sys.argv[3])
import ota_sign
ota_sign.install_pub(sys.argv[2], 1, src=sys.argv[1])
assert ota_sign.compiled_pubkeys(sys.argv[1]) == [sys.argv[2]]
ota_sign.install_pub(sys.argv[2], 2, src=sys.argv[1])
assert ota_sign.compiled_pubkeys(sys.argv[1]) == [sys.argv[2], sys.argv[2]]
EOF
chk "install writes OTA_PUBKEY / OTA_PUBKEY2 (on a copy)" "[ $? = 0 ] && cmp -s $R/src/ota_net.inc.c $T/src_backup.c"
chk "the repository source keeps one well-formed public key" "grep -q 'OTA_PUBKEY_HEX \"[0-9a-f]\{64\}\" /\* OTA_PUBKEY \*/' $R/src/ota_net.inc.c"

echo; echo "=== 3. flow (worker + hibynet x86 + local server + real installer under dash)"
# hibynet for the PC from the current source (deps of tools/wsl/zbuild_net.sh x86; bin/ untouched)
if [ ! -x $NET ] || [ $R/src/hibynet.c -nt $NET ]; then
    N=$HOME/hiby/net
    gcc -Os -Wno-format-truncation -Wall -Wno-unused-function -I$HOME/hiby/third -I$N/x86/include -o $NET $R/src/hibynet.c         -L$N/x86/lib -lmbedtls -lmbedx509 -lmbedcrypto -ljpeg -lm 2>&1 | tail -3
fi
if [ ! -x $NET ] || ! strings $NET | grep -q HIBYNET_RESUME; then
    echo "  hibynet x86 missing or without HIBYNET_RESUME: tools/wsl/zbuild_net.sh x86"; bad "hibynet-x86"; echo "$fails FAILURES"; exit 1
fi
SIM=$T/dev; SRV=$T/srv
mkdir -p $SIM/bin $SIM/sd $SIM/data/hibymod $SIM/tmp $SIM/flags $SRV
for d in layout/theme3/launcher litegui/theme3/launcher layout/theme3/topbar; do mkdir -p $SIM/res/$d; done
printf '{"stock":1}' > $SIM/res/layout/theme3/launcher/hiby_launcher_apps.view
printf '{"stock_topbar":1}' > $SIM/res/layout/theme3/topbar/topbar.view
printf 'stock view' > $SIM/res/layout/theme3/hiby_main_category.view
printf 'PNG stock' > $SIM/res/litegui/theme3/launcher/music.png
printf 'PNG stock2' > $SIM/res/litegui/theme3/launcher/book.png
mkdir -p $SIM/proot$SIM; cp -a $SIM/res $SIM/proot$SIM/res
cat > $SIM/bin/mount <<X
#!/bin/sh
[ "\$1" = "-o" ] && [ "\$2" = bind ] || exit 1
rmdir "\$4" 2>/dev/null; ln -sfn "\$3" "\$4"
X
printf '#!/bin/sh\nrm -f "$1"; mkdir -p "$1"\n' > $SIM/bin/umount
printf '#!/bin/sh\nexit 0\n' > $SIM/bin/sync
chmod +x $SIM/bin/*
echo "/dev/mmcblk0 $SIM/sd exfat rw 0 0" > $SIM/mounts
echo 80 > $SIM/batt; echo 0 > $SIM/usb
printf '#!/bin/sh\ncase "$1" in status) echo "HiByOS 0.9.0" ;; esac\n' > $SIM/hibymod_cli
H=$SIM/data/hibymod
export PATH=$SIM/bin:$PATH HIBYOS_HM=$H HIBYOS_DATA=$SIM/data HIBYOS_SD=$SIM/sd HIBYOS_SD_RE=" $SIM/sd " \
    HIBYOS_RES=$SIM/res HIBYOS_MOUNTS=$SIM/mounts HIBYOS_ROOTFS=$SIM/proot HIBYOS_TMP=$SIM/tmp HIBYOS_WAIT=1 HIBYOS_FLAGS=$SIM/flags \
    HIBYOS_ROOTBIN=$R/overlay/usr/bin
export OTA_SD=$SIM/sd OTA_HM=$H OTA_NET=$NET OTA_SH=/usr/bin/dash OTA_INSTALLER=$R/overlay/usr/bin/hibyos_install.sh \
    OTA_MOUNTS=$SIM/mounts OTA_BATT=$SIM/batt OTA_USB=$SIM/usb OTA_CLI=$SIM/hibymod_cli
dash -n $R/overlay/usr/bin/hibyos_install.sh && ok "installer: dash -n" || bad "installer syntax"

$PY $R/tests/ota_httpd.py $SRV $T/port $T/http.log & SRVPID=$!
trap 'kill $SRVPID 2>/dev/null' EXIT
for i in $(seq 50); do [ -s $T/port ] && break; sleep 0.1; done
PORT=$(cat $T/port)
export OTA_URL=http://127.0.0.1:$PORT/latest/download/hibyos-update.json
cat > $T/cl.json <<'EOF'
{"en": ["Faster scrolling", "New Software Update page"], "es": ["Desplazamiento más fluido", "Nueva página de Actualización de software"],
 "fr": "Défilement plus fluide.", "de": "Schnelleres Scrollen."}
EOF
P=$T/payloads; mkdir -p $P
$PY $R/tests/ota_mkpayload.py $P/0.9.0 0.9.0 --shell 0 --theme 0 --override
$PY $R/tests/ota_mkpayload.py $P/0.9.1 0.9.1 --shell 1 --theme 0 --override
$PY $R/tests/ota_mkpayload.py $P/0.9.2 0.9.2 --shell 1 --theme 2 --override
$PY $R/tests/ota_mkpayload.py $P/0.9.3 0.9.3 --shell 3 --theme 2 --no-launcher-view
release() {   # version payload_dir [prev] [key]: publishes on the local server
    local prev=""; [ -n "${3:-}" ] && prev="--prev-payload $3"
    $PY $R/tools/ota_release.py --key ${4:-$FIX/test1.key} --changelog $T/cl.json --version $1 --payload $2 $prev \
        --url-base http://127.0.0.1:$PORT/dl/ --out $T/rel/$1 --allow-key-mismatch >$T/rel_$1.txt || { cat $T/rel_$1.txt; return 1; }
    cp $T/rel/$1/hibyos-payload-$1.tar $T/rel/$1/hibyos-update.json $SRV/
}
op() { $T/ota_test op "$@" 2>>$T/worker.log | tail -1; }
field() { echo "$1" | tr ' ' '\n' | grep "^$2=" | cut -d= -f2; }
sdver() { head -1 $SIM/sd/HiByOS/payload/VERSION 2>/dev/null; }
prevver() { head -1 $SIM/sd/HiByOS/ota/previous/payload/VERSION 2>/dev/null; }

# device with 0.9.0 installed the usual way (SD payload + installer)
mkdir -p $SIM/sd/HiByOS; cp -a $P/0.9.0 $SIM/sd/HiByOS/payload
dash $R/overlay/usr/bin/hibyos_install.sh install >/dev/null
chk "0.9.0 installed by the installer" "[ \"\$(cat $H/payload_version)\" = 0.9.0 ]"
printf 'lang en\n' > $H/shell/launcher.conf; echo 'cpu 1' > $H/power.conf; mkdir -p $H/apps/org.x $H/archivos
printf 'app' > $H/apps/org.x/app.json; printf 'mine' > $H/archivos/nota.txt; printf 'x' > $H/shell/wall_custom.raw
userstate() { grep -q 'lang en' $H/shell/launcher.conf && [ "$(cat $H/power.conf)" = 'cpu 1' ] && [ -f $H/apps/org.x/app.json ] && [ "$(cat $H/archivos/nota.txt)" = mine ] && [ -f $H/shell/wall_custom.raw ]; }

echo "  -- release 0.9.1 (interface-only change)"
release 0.9.1 $P/0.9.1 $P/0.9.0 || bad "ota_release 0.9.1"
chk "ota_release: restart=shell computed from the manifests" "grep -q '\"restart\": \"shell\"' $T/rel/0.9.1/hibyos-update.json"
chk "ota_release: gh command printed (not run)" "grep -q 'gh release create v0.9.1' $T/rel_0.9.1.txt"
chk "ota_release: reproducible tar" "cp $T/rel/0.9.1/hibyos-payload-0.9.1.tar $T/a.tar && release 0.9.1 $P/0.9.1 $P/0.9.0 && cmp -s $T/a.tar $T/rel/0.9.1/hibyos-payload-0.9.1.tar"
$PY $R/tools/ota_release.py --key $FIX/test1.key --changelog $T/cl.json --version 0.9.1 --payload $P/0.9.1 --out $T/rel/x >/dev/null 2>&1
chk "ota_release refuses a key that is not compiled in" "[ $? != 0 ]"
echo '{"en": "x"}' > $T/cl_bad.json
$PY $R/tools/ota_release.py --key $FIX/test1.key --changelog $T/cl_bad.json --version 0.9.1 --payload $P/0.9.1 --out $T/rel/x --allow-key-mismatch >/dev/null 2>&1
chk "ota_release requires en/es/fr changelogs" "[ $? != 0 ]"
$PY $R/tools/ota_release.py --key $FIX/test1.key --changelog $T/cl.json --version 0.9.5 --payload $P/0.9.1 --out $T/rel/x --allow-key-mismatch >/dev/null 2>&1
chk "ota_release refuses a payload of another version" "[ $? != 0 ]"

printf '#!/bin/sh\nls /proc/self/fd > %s/fds.txt\nexec %s "$@"\n' $T $NET > $T/netwrap; chmod +x $T/netwrap
exec 7>$T/leak.txt                                   # an fd the test process holds (like the shim socket)
r=$(OTA_NET=$T/netwrap op check); echo "     $r"
exec 7>&-
chk "children get no inherited descriptors (only 0-2, ls's own 3, dash's script fd 10)" "[ -s $T/fds.txt ] && ! grep -qvxE '0|1|2|3|10' $T/fds.txt"
chk "check: 0.9.1 available" "[ \"$(field "$r" state)\" = AVAILABLE ] && [ \"$(field "$r" installed)\" = 0.9.0 ]"
chk "check went through the /latest/download redirect" "grep -q '^GET /latest/download/hibyos-update.json - 302' $T/http.log && grep -q '^GET /dl/hibyos-update.json - 200' $T/http.log"
SIZE=$(stat -c %s $SRV/hibyos-payload-0.9.1.tar)
echo $((SIZE / 3)) > $SRV/.cut
r=$(op install); echo "     $r"
PART=$SIM/sd/HiByOS/ota/hibyos-payload-0.9.1.tar.part
chk "dropped connection: paused (NET), .part kept" "[ \"$(field "$r" state)\" = PAUSED ] && [ \"$(field "$r" err)\" = NET ] && [ -s $PART ]"
chk "nothing installed yet" "[ \"\$(cat $H/payload_version)\" = 0.9.0 ] && [ \"\$(sdver)\" = 0.9.0 ]"
r=$(op load); echo "     $r"
chk "reopening the page: paused with the partial size" "[ \"$(field "$r" state)\" = PAUSED ] && [ \"$(field "$r" got)\" = \$(stat -c %s $PART) ]"
echo 30 > $SRV/.slow
r=$(op install pause-at $((SIZE / 2))); echo "     $r"
rm -f $SRV/.slow
chk "user pause: PAUSED, more bytes kept" "[ \"$(field "$r" state)\" = PAUSED ] && [ \$(stat -c %s $PART) -gt $((SIZE / 3)) ]"
chk "the pause resumed with Range" "grep -q '^GET /dl/hibyos-payload-0.9.1.tar bytes=$((SIZE / 3))- 206' $T/http.log"
GOT=$(stat -c %s $PART)
r=$(op install); echo "     $r"
chk "resumed again with Range from the kept bytes" "grep -q \"^GET /dl/hibyos-payload-0.9.1.tar bytes=$GOT- 206\" $T/http.log"
chk "installed 0.9.1, interface restart only" "[ \"$(field "$r" state)\" = DONE ] && [ \"$(field "$r" restart)\" = 1 ] && [ \"\$(cat $H/payload_version)\" = 0.9.1 ]"
chk "new shell binary in place" "cmp -s $P/0.9.1/bin/hibyshell $H/shell/hibyshell"
chk "SD payload = 0.9.1, ota/previous = 0.9.0 (boot will not reinstall the old one)" "[ \"\$(sdver)\" = 0.9.1 ] && [ \"\$(prevver)\" = 0.9.0 ]"
chk "download and staging removed" "[ ! -e $PART ] && [ ! -e $SIM/sd/HiByOS/ota/hibyos-payload-0.9.1.tar ] && [ ! -e $SIM/sd/HiByOS/ota/stage ]"
chk "user state untouched" "userstate"
touch $T/stamp; sleep 1
dash $R/overlay/usr/bin/hibyos_install.sh boot; dash $R/overlay/usr/bin/hibyos_install.sh late
chk "next boot: nothing to install (no writes)" "[ -z \"\$(cd $SIM/data && find . -type f -newer $T/stamp)\" ]"
r=$(op check); echo "     $r"
chk "check again: up to date" "[ \"$(field "$r" state)\" = UPTODATE ]"

echo "  -- sys/ override of the installer"
chk "sysok: override valid" "dash $R/overlay/usr/bin/hibyos_install.sh sysok hibyos_install.sh"
: > $SIM/tmp/override.log
dash $R/overlay/usr/bin/hibyos_install.sh status >/dev/null
dash $R/overlay/usr/bin/hibyos_install.sh boot
chk "rootfs installer delegates to the valid override (boot)" "grep -q 'OVERRIDE boot' $SIM/tmp/override.log"
chk "status/sysok are never delegated" "! grep -q 'OVERRIDE status' $SIM/tmp/override.log"
: > $SIM/tmp/override.log; HIBYOS_NO_OVERRIDE=1 dash $R/overlay/usr/bin/hibyos_install.sh boot
chk "HIBYOS_NO_OVERRIDE=1: built-in one" "[ ! -s $SIM/tmp/override.log ]"
echo 2 > $SIM/flags/.hibymod_bootfail
chk "2 failed boots: override ignored" "! dash $R/overlay/usr/bin/hibyos_install.sh sysok hibyos_install.sh"
echo 0 > $SIM/flags/.hibymod_bootfail; touch $SIM/flags/.hibymod_safe
chk "safe mode: override ignored" "! dash $R/overlay/usr/bin/hibyos_install.sh sysok hibyos_install.sh"
rm -f $SIM/flags/.hibymod_safe; cp $H/sys/hibyos_install.sh $T/ov.bak; echo '# tampered' >> $H/sys/hibyos_install.sh
chk "tampered override (sha256 mismatch): ignored" "! dash $R/overlay/usr/bin/hibyos_install.sh sysok hibyos_install.sh"
cp $T/ov.bak $H/sys/hibyos_install.sh
cp $H/sys/MANIFEST.sha256 $T/man.bak
ROOTREV=$(sed -n 's/^# HIBYOS_SYS_REV=\([0-9][0-9]*\).*/\1/p' $R/overlay/usr/bin/hibyos_install.sh | head -n 1)
for rev in $ROOTREV $((ROOTREV - 1)) 0; do   # a valid pair whose revision is not newer than the rootfs one (old payload, newer firmware)
    sed "s/^# HIBYOS_SYS_REV=.*/# HIBYOS_SYS_REV=$rev/" $T/ov.bak > $H/sys/hibyos_install.sh
    echo "$(sha256sum < $H/sys/hibyos_install.sh | cut -d' ' -f1)  hibyos_install.sh" > $H/sys/MANIFEST.sha256
    chk "valid override with revision $rev (rootfs has $ROOTREV): ignored" "! dash $R/overlay/usr/bin/hibyos_install.sh sysok hibyos_install.sh"
done
cp $T/ov.bak $H/sys/hibyos_install.sh; cp $T/man.bak $H/sys/MANIFEST.sha256
chk "and the newer one is valid again" "dash $R/overlay/usr/bin/hibyos_install.sh sysok hibyos_install.sh"
chk "unknown name: ignored" "! dash $R/overlay/usr/bin/hibyos_install.sh sysok hiby_player.sh"

echo "  -- rollback"
r=$(op rollback); echo "     $r"
chk "restore previous: 0.9.0 installed, SD and previous swapped" "[ \"$(field "$r" state)\" = DONE ] && [ \"\$(cat $H/payload_version)\" = 0.9.0 ] && [ \"\$(sdver)\" = 0.9.0 ] && [ \"\$(prevver)\" = 0.9.1 ]"
chk "old shell binary back, user state untouched" "cmp -s $P/0.9.0/bin/hibyshell $H/shell/hibyshell && userstate"
r=$(op rollback); echo "     $r"
chk "and back again to 0.9.1" "[ \"$(field "$r" state)\" = DONE ] && [ \"\$(cat $H/payload_version)\" = 0.9.1 ] && [ \"\$(prevver)\" = 0.9.0 ]"

echo "  -- release 0.9.2 (theme change -> reboot), tampered tar first"
release 0.9.2 $P/0.9.2 $P/0.9.1 || bad "ota_release 0.9.2"
cp $SRV/hibyos-payload-0.9.2.tar $T/good092.tar
$PY -c "import sys; p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[700]^=1; open(p,'wb').write(b)" $SRV/hibyos-payload-0.9.2.tar
r=$(op check); echo "     $r"
r=$(op install); echo "     $r"
chk "tampered tar (same size): SHA error, nothing changed" "[ \"$(field "$r" state)\" = ERROR ] && [ \"$(field "$r" err)\" = SHA ] && [ \"\$(cat $H/payload_version)\" = 0.9.1 ] && [ \"\$(sdver)\" = 0.9.1 ]"
chk "the bad download was deleted" "[ ! -e $SIM/sd/HiByOS/ota/hibyos-payload-0.9.2.tar ]"
cp $T/good092.tar $SRV/hibyos-payload-0.9.2.tar
echo 10 > $SIM/batt
r=$(op install); echo "     $r"
chk "battery 10% and not charging: refused" "[ \"$(field "$r" err)\" = BATT ] && [ ! -e $SIM/sd/HiByOS/ota/hibyos-payload-0.9.2.tar.part ]"
echo 1 > $SIM/usb
grep -v sd $SIM/mounts > $SIM/m2; cp $SIM/mounts $SIM/m1; cp $SIM/m2 $SIM/mounts
r=$(op install); echo "     $r"
chk "charging is enough for the battery rule; no SD (USB storage): refused" "[ \"$(field "$r" err)\" = NOSD ]"
cp $SIM/m1 $SIM/mounts
r=$(op install); echo "     $r"
chk "0.9.2 installed, reboot needed" "[ \"$(field "$r" state)\" = DONE ] && [ \"$(field "$r" restart)\" = 2 ] && [ \"\$(cat $H/payload_version)\" = 0.9.2 ]"
chk "our theme file installed, user state untouched" "grep -q '\"ours\":2' $H/theme/layout/theme3/launcher/hiby_launcher_apps.view && userstate"
echo 80 > $SIM/batt; echo 0 > $SIM/usb

echo "  -- forged update descriptions"
cp $SRV/hibyos-update.json $T/u.json
sed 's/Faster scrolling/Install from evil.example/' $T/u.json > $SRV/hibyos-update.json
r=$(op check); echo "     $r"
chk "tampered JSON: SIG error" "[ \"$(field "$r" state)\" = ERROR ] && [ \"$(field "$r" err)\" = SIG ]"
chk "the verified copy on the SD is kept" "grep -q 'Faster scrolling' $SIM/sd/HiByOS/ota/hibyos-update.json && [ ! -e $SIM/sd/HiByOS/ota/hibyos-update.json.new ]"
$PY $R/tests/ota_mkpayload.py $P/0.9.9 0.9.9 --shell 9 && release 0.9.9 $P/0.9.9 "" $FIX/test2.key
r=$(op check); echo "     $r"
chk "signed with another key: SIG error" "[ \"$(field "$r" err)\" = SIG ]"
rm -f $SRV/hibyos-update.json
r=$(op check); echo "     $r"
chk "server without the file (404): NET error" "[ \"$(field "$r" err)\" = NET ]"

echo "  -- downgrade"
release 0.9.1 $P/0.9.1
r=$(op check); echo "     $r"
chk "older release than installed: NEWER" "[ \"$(field "$r" state)\" = NEWER ]"
r=$(op install); echo "     $r"
chk "install without permission: refused (DOWNGRADE)" "[ \"$(field "$r" err)\" = DOWNGRADE ] && [ \"\$(cat $H/payload_version)\" = 0.9.2 ]"
r=$(op install-down); echo "     $r"
chk "explicit downgrade: 0.9.1 installed" "[ \"$(field "$r" state)\" = DONE ] && [ \"\$(cat $H/payload_version)\" = 0.9.1 ] && [ \"\$(prevver)\" = 0.9.2 ]"

echo "  -- an install that fails half-way restores the previous version"
release 0.9.3 $P/0.9.3
r=$(op check); echo "     $r"
r=$(op install); echo "     $r"
chk "INSTALL error" "[ \"$(field "$r" state)\" = ERROR ] && [ \"$(field "$r" err)\" = INSTALL ]"
chk "0.9.1 installed again (files, payload_version, SD payload)" "[ \"\$(cat $H/payload_version)\" = 0.9.1 ] && cmp -s $P/0.9.1/bin/hibyshell $H/shell/hibyshell && [ \"\$(sdver)\" = 0.9.1 ]"
chk "the previous version (0.9.2) is still the rollback target" "[ \"\$(prevver)\" = 0.9.2 ] && [ ! -e $SIM/sd/HiByOS/ota/previous.old ]"
chk "built-in installer retried after the override failed" "grep -q 'again with the built-in one' $T/worker.log"
chk "user state untouched" "userstate"

echo "  -- needs firmware / min_version"
$PY $R/tests/ota_mkpayload.py $P/1.0.0 1.0.0 --shell 4
$PY $R/tools/ota_release.py --key $FIX/test1.key --changelog $T/cl.json --version 1.0.0 --payload $P/1.0.0 --needs-firmware \
    --url-base http://127.0.0.1:$PORT/dl/ --out $T/rel/1.0.0 --allow-key-mismatch >/dev/null && cp $T/rel/1.0.0/hibyos-* $SRV/
r=$(op check); echo "     $r"
r=$(op install); echo "     $r"
chk "needs_firmware: refused with FIRMWARE (UI shows the Patcher text)" "[ \"$(field "$r" err)\" = FIRMWARE ] && [ \"\$(cat $H/payload_version)\" = 0.9.1 ]"
$PY $R/tools/ota_release.py --key $FIX/test1.key --changelog $T/cl.json --version 1.0.0 --payload $P/1.0.0 --min-version 0.9.5 \
    --url-base http://127.0.0.1:$PORT/dl/ --out $T/rel/1.0.0 --allow-key-mismatch >/dev/null && cp $T/rel/1.0.0/hibyos-* $SRV/
r=$(op check); r=$(op install); echo "     $r"
chk "installed older than min_version: refused (MINVER)" "[ \"$(field "$r" err)\" = MINVER ]"

echo; echo "worker log: $T/worker.log, server log: $T/http.log"
[ $fails = 0 ] && echo "ALL OK" || echo "$fails FAILURES"
exit $fails
