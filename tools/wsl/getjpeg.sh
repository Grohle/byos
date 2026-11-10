set -e
. "$(dirname "$0")/fetch.sh"
cd ~/hiby/third
# libjpeg 9f (Independent JPEG Group), pinned by SHA-256
fetch_verify https://www.ijg.org/files/jpegsrc.v9f.tar.gz 04705c110cb2469caa79fb71fba3d7bf834914706e9641a4589485c1f832565b jpegsrc.v9f.tar.gz
tar xzf jpegsrc.v9f.tar.gz
cd jpeg-9f
cp jconfig.txt jconfig.h 2>/dev/null || true
ls | head -50 | tr '\n' ' '
