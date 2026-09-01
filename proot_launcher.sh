#!/bin/bash
# proot launcher pro testrootfs
# Binduje hostovský / do /parrot uvnitř prootu

set -e

ROOTFS="$(cd "$(dirname "$0")/testrootfs" && pwd)"

exec proot \
  -r "$ROOTFS" \
  -b /:/parrot \
  -b /dev \
  -b /proc \
  -b /sys \
  -b /data \
  -b /sdcard \
  -b /mnt \
  -b /storage \
  -b /res \
  -b /config \
  --sysvipc=auto \
  --link2symlink \
  /bin/bash -l
