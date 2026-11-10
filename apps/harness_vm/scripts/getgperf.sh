cd ~/hiby/vm/debs/x; apt-get download gperf 2>&1 | tail -1; for f in gperf*.deb; do dpkg -x $f ~/hiby/vm/xtc; done; ls ~/hiby/vm/xtc/usr/bin/gperf
