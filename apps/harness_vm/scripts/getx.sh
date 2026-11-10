set -e
X=~/hiby/vm/xtc; mkdir -p $X ~/hiby/vm/debs/x; cd ~/hiby/vm/debs/x
need=""
for p in gcc-13-mipsel-linux-gnu cpp-13-mipsel-linux-gnu gcc-13-mipsel-linux-gnu-base binutils-mipsel-linux-gnu libgcc-13-dev-mipsel-cross flex bison m4 libfl2 libfl-dev libelf-dev libssl-dev libisl23 libmpc3 libmpfr6 libcc1-0 libgmp10; do
  dpkg -s $p >/dev/null 2>&1 || need="$need $p"; done
echo NEED $need
apt-get download $need 2>&1 | tail -2
for f in *.deb; do dpkg -x $f $X; done
ls $X/usr/bin | head -40
