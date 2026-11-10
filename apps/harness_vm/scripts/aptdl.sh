#!/bin/bash
A=~/hiby/vm/apt
export APT_CONFIG=$A/apt.conf
apt-get -y -d -o Debug::NoLocking=1 install linux-image-6.1.0-50-4kc-malta busybox-static netsurf-fb links2 w3m 2>&1 | tail -5
ls $A/var/cache/apt/archives/*.deb | wc -l
du -sh $A/var/cache/apt/archives
