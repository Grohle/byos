#!/bin/bash
# ByOS Desktop.exe launcher (desktop/windows/launcher.c): x86_64 Windows, GUI subsystem, with its icon,
# version information and manifest. The payload is appended later by tools/package_desktop.py.
#   bash tools/wsl/zbuild_desktop_exe.sh 0.9.0
set -e
. "$(dirname "$0")/fetch.sh"; zig_check   # pinned compiler: ziglang 0.17.1, see tools/wsl/fetch.sh
V=${1:?version, e.g. 0.9.0}
IFS=. read -r V1 V2 V3 <<<"$V"
# this checkout (also a git worktree), wherever it is
R=$(cd "$(dirname "$0")/../.." && pwd)
D=$R/desktop/windows
O=$R/out/desktop_exe
mkdir -p $O
# the resource compiler does not see -D: a copy of launcher.rc with the version written in, next
# to copies of the icon and the manifest
cp $D/byos.ico $D/launcher.manifest $D/launcher.c $O/
sed -e "s/BYOS_V1,BYOS_V2,BYOS_V3/$V1,$V2,$V3/g" -e "s/BYOS_VS/\"$V\"/g" $D/launcher.rc > $O/launcher.rc
cd $O
~/hiby/venv/bin/python -m ziglang cc -target x86_64-windows-gnu -O2 -s -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers   -DBYOS_VERSION="L\"$V\"" -Wl,--subsystem,windows -o launcher.exe launcher.c launcher.rc -luser32 -lgdi32 -lshell32
ls -la $O/launcher.exe
