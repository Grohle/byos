K=~/hiby/vm/kernel; C=$(ls $K/boot/config-*)
grep -E "UINPUT|CONFIG_INPUT_MISC=|CONFIG_9P|NET_9P|CONFIG_TMPFS=|CONFIG_SWAP=|CONFIG_FB_DEFERRED|CONFIG_HZ=" $C
find $K/lib/modules -name "*uinput*" -o -name "9p*.ko" -o -name "evdev.ko"
ls -la $K/boot/vmlinuz*; file $K/boot/vmlinuz*
