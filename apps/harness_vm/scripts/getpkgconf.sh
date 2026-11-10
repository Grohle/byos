cd ~/hiby/vm/debs/x
for f in pkgconf-bin*.deb libpkgconf3*.deb; do dpkg -x $f ~/hiby/vm/xtc; done
ln -sf pkgconf ~/hiby/vm/xtc/usr/bin/pkg-config
LD_LIBRARY_PATH=~/hiby/vm/xtc/usr/lib/x86_64-linux-gnu PKG_CONFIG_LIBDIR=~/hiby/vm/xsysroot/lib/pkgconfig ~/hiby/vm/xtc/usr/bin/pkg-config --static --libs libcurl
