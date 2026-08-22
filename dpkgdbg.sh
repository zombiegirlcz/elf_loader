#!/bin/bash
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
dpkg -i /var/cache/apt/archives/systemd_257.13-1~deb13u1_arm64.deb 2>&1 | grep -iE "merged|usr|error|fail|not have" | head -10
echo "=== preinst manual run:"
export DPKG_MAINTSCRIPT_PACKAGE=systemd DPKG_MAINTSCRIPT_NAME=preinst DPKG_MAINTSCRIPT_ARGS="upgrade 241-5" DPKG_ROOT=/ DPKG_ADMINDIR=/var/lib/dpkg
sh /var/lib/dpkg/info/systemd.preinst upgrade 241-5 2>&1 | tail -6
echo PREINST_RC=$?
