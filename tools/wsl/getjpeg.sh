set -e
cd ~/hiby/third
[ -f jpegsrc.v9f.tar.gz ] || curl -sSfL -o jpegsrc.v9f.tar.gz https://www.ijg.org/files/jpegsrc.v9f.tar.gz
tar xzf jpegsrc.v9f.tar.gz
cd jpeg-9f
cp jconfig.txt jconfig.h 2>/dev/null || true
ls | head -50 | tr '\n' ' '
