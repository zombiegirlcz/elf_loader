#!/bin/bash
# Rychlé testování konkrétních binárek přes ashell (na reálném Android zařízení)
# Použití: ./quick-test.sh grep sed awk ls cat

D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader

if [ $# -eq 0 ]; then
    echo "Usage: $0 <binary1> [binary2] ..."
    echo "Example: $0 grep sed awk ls cat"
    exit 1
fi

for bin_name in "$@"; do
    # Najdi binárku
    bin_path=$(find $R/bin $R/usr/bin $R/usr/sbin $R/sbin -name "$bin_name" -type f 2>/dev/null | head -1)
    
    if [ -z "$bin_path" ]; then
        echo "❌ NOT FOUND: $bin_name"
        continue
    fi
    
    echo "Testing: $bin_name ($bin_path)"
    
    # Testovací příkazy podle binárky - VŠECHNO přes ashell -c
    case "$bin_name" in
        grep)
            echo -n "  grep -q root /etc/passwd: "
            ashell -c "$L --ownall $bin_path -q root /etc/passwd" && echo "✅ PASS" || echo "❌ FAIL"
            ;;
        sed)
            echo -n "  sed -n '1p' /etc/hostname: "
            result=$(ashell -c "$L --ownall $bin_path -n '1p' /etc/hostname" 2>&1)
            if [ $? -eq 0 ]; then echo "✅ PASS ($result)"; else echo "❌ FAIL ($result)"; fi
            ;;
        awk)
            echo -n "  awk 'BEGIN{print 1+2}': "
            result=$(ashell -c "$L --ownall $bin_path 'BEGIN{print 1+2}'" 2>&1)
            if [ "$result" = "3" ]; then echo "✅ PASS ($result)"; else echo "❌ FAIL ($result)"; fi
            ;;
        ls)
            echo -n "  ls /etc: "
            ashell -c "$L --ownall $bin_path /etc" > /dev/null 2>&1 && echo "✅ PASS" || echo "❌ FAIL"
            ;;
        cat)
            echo -n "  cat /etc/hostname: "
            result=$(ashell -c "$L --ownall $bin_path /etc/hostname" 2>&1)
            if [ $? -eq 0 ]; then echo "✅ PASS ($result)"; else echo "❌ FAIL ($result)"; fi
            ;;
        wc)
            echo -n "  wc -c /etc/hostname: "
            result=$(ashell -c "$L --ownall $bin_path -c /etc/hostname" 2>&1)
            if [ $? -eq 0 ]; then echo "✅ PASS ($result)"; else echo "❌ FAIL ($result)"; fi
            ;;
        find)
            echo -n "  find /etc -maxdepth 1: "
            ashell -c "$L --ownall $bin_path /etc -maxdepth 1" > /dev/null 2>&1 && echo "✅ PASS" || echo "❌ FAIL"
            ;;
        *)
            echo -n "  --help: "
            ashell -c "$L --ownall $bin_path --help" > /dev/null 2>&1 && echo "✅ PASS" || echo "❌ FAIL"
            ;;
    esac
done
