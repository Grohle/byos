which flex bison bc perl make gcc xz
apt-cache policy gcc-mipsel-linux-gnu gcc-13-mipsel-linux-gnu binutils-mipsel-linux-gnu | grep -E "^[a-z]|Cand"
apt-cache depends gcc-13-mipsel-linux-gnu | head -20
curl -s https://www.kernel.org/releases.json | python3 -c "import json,sys; d=json.load(sys.stdin); print([ (r['version']) for r in d['releases'] if r['moniker'] in ('longterm','stable')])"
