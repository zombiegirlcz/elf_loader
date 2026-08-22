#!/bin/bash
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export DEBIAN_FRONTEND=noninteractive
dpkg --configure -a
apt-get install -y libpam-systemd dbus-user-session 2>&1 | tail -2
apt-get -f install -y 2>&1 | tail -2
apt-get check && echo APT_CHECK_OK
