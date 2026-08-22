#!/bin/bash
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export DEBIAN_FRONTEND=noninteractive
set -e
rm -rf /tmp/sysd && mkdir -p /tmp/sysd
dpkg-deb -R /var/cache/apt/archives/systemd_257.13-1~deb13u1_arm64.deb /tmp/sysd
printf '#!/bin/sh\nexit 0\n' > /tmp/sysd/DEBIAN/preinst
chmod 755 /tmp/sysd/DEBIAN/preinst
dpkg-deb -b /tmp/sysd /tmp/systemd-fixed.deb
dpkg -i /tmp/systemd-fixed.deb
