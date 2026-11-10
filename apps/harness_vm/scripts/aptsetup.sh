#!/bin/bash
set -e
A=~/hiby/vm/apt
mkdir -p $A/etc/apt/sources.list.d $A/etc/apt/preferences.d $A/etc/apt/apt.conf.d $A/var/lib/apt/lists/partial $A/var/cache/apt/archives/partial $A/var/lib/dpkg
touch $A/var/lib/dpkg/status
echo "deb [trusted=yes] http://deb.debian.org/debian bookworm main" > $A/etc/apt/sources.list
cat > $A/apt.conf <<EOC
Dir "$A/";
Dir::State "$A/var/lib/apt";
Dir::State::status "$A/var/lib/dpkg/status";
Dir::Cache "$A/var/cache/apt";
Dir::Etc "$A/etc/apt";
APT::Architecture "mipsel";
APT::Architectures { "mipsel"; };
APT::Install-Recommends "false";
Acquire::AllowInsecureRepositories "true";
Acquire::Languages "none";
EOC
APT_CONFIG=$A/apt.conf apt-get update 2>&1 | tail -3
APT_CONFIG=$A/apt.conf apt-cache search malta | grep linux-image
APT_CONFIG=$A/apt.conf apt-cache policy netsurf-fb links2 busybox-static | grep -E "^[a-z]|Candidate"
