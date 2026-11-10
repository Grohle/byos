set -e
H=~/hiby/vm/hostsysroot; mkdir -p ~/hiby/vm/src/hostdeps; cd ~/hiby/vm/src/hostdeps
D=~/hiby/vm/dl/deps
[ -f $H/lib/libz.a ] || { rm -rf zlib-1.3.1; tar xf $D/zlib-1.3.1.tar.gz; cd zlib-1.3.1; CC=gcc ./configure --static --prefix=$H >/dev/null; make -j16 >/dev/null; make install >/dev/null; cd ..; }
[ -f $H/lib/libpng.a ] || { rm -rf libpng-1.6.44; tar xf $D/libpng-1.6.44.tar.xz; cd libpng-1.6.44; CC=gcc CPPFLAGS=-I$H/include LDFLAGS=-L$H/lib ./configure --prefix=$H --disable-shared >/dev/null; make -j16 >/dev/null; make install >/dev/null; cd ..; }
ls $H/lib
