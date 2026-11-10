set -e
cd ~/hiby/vm; mkdir -p tdlib; cd tdlib
[ -d venv ] || python3 -m venv venv
./venv/bin/pip -q install python-telegram 2>&1 | tail -2
find venv -name "libtdjson*" -exec ls -la {} \;
