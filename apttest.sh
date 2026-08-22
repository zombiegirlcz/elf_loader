#!/bin/bash
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export DEBIAN_FRONTEND=noninteractive
apt-get install -y sl tree 2>&1 | tail -3
echo "---test:"
echo "" | sl 2>/dev/null | head -1 && echo SL_RUNS
tree --version 2>&1 | head -1
apt-get purge -y sl tree 2>&1 | tail -1
