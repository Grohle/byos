set -e
N=~/hiby/vm/src/netsurf-all-3.11/netsurf
O=~/hiby/vm/share/netsurf; rm -rf $O; mkdir -p $O/res
cp $N/nsfb $O/nsfb
cp -rL $N/frontends/framebuffer/res/. $O/res/ 2>/dev/null || true
rm -rf $O/res/de $O/res/fr $O/res/it $O/res/ja $O/res/nl $O/res/zh_CN
cat > $O/Choices <<'EOC'
fb_osk:1
fb_toolbar_size:28
fb_furniture_size:16
fb_depth:16
window_width:320
window_height:480
memory_cache_size:1048576
disc_cache_size:0
max_fetchers:3
max_fetchers_per_host:2
max_cached_fetch_handles:2
enable_javascript:0
block_advertisements:1
homepage_url:about:blank
EOC
cat > $O/run.sh <<'EOC'
#!/bin/sh
# Lanza NetSurf en el HiBy (o en el arnes): fb0 + touch + OSK
D=$(cd "$(dirname "$0")" && pwd)
export HOME=${HOME:-/tmp}; mkdir -p $HOME/.netsurf; [ -f $HOME/.netsurf/Choices ] || cp $D/Choices $HOME/.netsurf/Choices
export NETSURFRES=$D/res/
exec $D/nsfb -f linux -w 320 -h 480 -b 16 "${1:-about:blank}"
EOC
chmod +x $O/run.sh
du -sh $O; ls -la $O
