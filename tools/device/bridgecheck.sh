#!/bin/sh
# Estado del puente de entrada de hibymod_disp.so v3
p=$(pidof hiby_player)
echo "--- fds de entrada del reproductor"
ls -l /proc/$p/fd | grep -E 'input|eventfd|uinput'
echo "--- dispositivos"
grep -E 'Name|Handlers' /proc/bus/input/devices
echo "--- estado (magic held waiting yoffset frames input_mode view_seq)"
od -An -tu4 -N28 /dev/shm/hibymod_fbstate
dd if=/dev/shm/hibymod_fbstate bs=1 skip=28 count=96 2>/dev/null | tr -d '\0'; echo
ls -la /tmp/hibymod_input.sock
