#!/bin/sh
# READ-ONLY probe (no writes)
echo "== amixer"; amixer -c 0 contents 2>&1
echo "== cards"; cat /proc/asound/cards; ls /proc/asound/card0
echo "== pcm status"; cat /proc/asound/card0/pcm0p/sub0/hw_params 2>&1
echo "== switch"; for s in /sys/class/switch/*; do echo "$s: $(cat $s/state 2>/dev/null) $(cat $s/name 2>/dev/null)"; done
echo "== i2c"; ls /sys/bus/i2c/devices/; ls -la /sys/bus/i2c/devices/3-0048/ 2>&1
echo "== params es9219"; for p in /sys/module/codec_es9219/parameters/*; do echo "$p=$(cat $p)"; done
echo "== params card"; for p in /sys/module/x1600_hiby_r3ii_sound_card/parameters/*; do echo "$p=$(cat $p)"; done
echo "== hbc3000"; ls /sys/module/sa_sound_hbc3000/parameters/ 2>&1; ls /sys/devices/platform | head -80
echo "== data"; ls -la /usr/data/ | head -60
echo "== menu_cfg"; ls -la /data/menu_cfg 2>&1; cat /data/menu_cfg 2>&1 | head -50
echo "== region"; cat /usr/data/region 2>&1
echo "== alsa.conf"; cat /usr/data/alsa.conf 2>&1 | head -30
echo "== user.ini"; ls -la /usr/data/user.ini; hexdump -C /usr/data/user.ini 2>/dev/null | head -60 || od -A x -t x1z /usr/data/user.ini | head -60
echo "== dmesg"; dmesg | grep -i -E 'es9219|hbc3000|sass|sound|codec' | tail -40
echo "== regulators"; ls /sys/class/regulator/ 2>&1; for r in /sys/class/regulator/*; do echo "$r $(cat $r/name 2>/dev/null) $(cat $r/state 2>/dev/null) $(cat $r/microvolts 2>/dev/null)"; done
echo "== power supply"; ls /sys/class/power_supply/; cat /sys/class/power_supply/usb/constant_charge_current_max /sys/class/power_supply/usb/charge_control_limit_max 2>&1
