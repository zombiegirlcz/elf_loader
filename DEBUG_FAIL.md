# DEBUG_FAIL.md — known failures / limits

Tento soubor zaznamenává konkrétní binárky a testy, které padají nebo timeoutují pod `elf_loader --ownall` přes `ashell -c`. Cíl je postupně rozšiřovat seznam, aby bylo jasné, co nefunguje a proč.

## Format

- `bin` — název binárky
- `cmd` — přesný testovací příkaz
- `rc` — exit kód / signál
- `note` — poznámka

## 2026-09-04

- `grep` | `cmd=grep -q root /etc/passwd` | `rc=124` | `note=TIMEOUT 5s; repeatable on device`
- `awk` | `cmd=awk 'BEGIN{print 1+2}'` | `rc=124` | `note=TIMEOUT 5s; repeatable on device`
- `patch` | `cmd=echo test > /tmp/test_patch && patch -p0 < /dev/null` | `rc=124` | `note=TIMEOUT 5s; likely stdin-related or patch init hang`
- `gawk` | `cmd=gawk 'BEGIN{print 1+2}'` | `rc=124` | `note=TIMEOUT 5s; repeatable on device`
- `mawk` | `cmd=mawk 'BEGIN{print 1+2}'` | `rc=124` | `note=TIMEOUT 5s; repeatable on device`
- `xargs` | `cmd=echo 'a' | xargs echo` | `rc=124` | `note=TIMEOUT 5s; stdin pipe path under loader`

## 2026-09-04 (extended run)

- `pmap` | `cmd=pmap -x 1 2>/dev/null | head -5` | `rc=0` | `note=PASS`
- `gdbus` | `cmd=gdbus --version 2>&1 | head -1` | `rc=0` | `note=PASS`
- `hostnamectl` | `cmd=hostnamectl status 2>&1 | head -5` | `rc=0` | `note=PASS`
- `python -m kaggle` | `rc=0` | `note=PASS (--help)`
- `python -m modal` | `rc=0` | `note=PASS (--help)`
- `python -m yt_dlp` | `rc=0` | `note=PASS (--help)`
- `python -m huggingface_hub` | `rc=0` | `note=PASS (--help)`

## 2026-09-04 (extended run 3)

- `ping6` | `cmd=ping6 -c 1 -W 1 127.0.0.1` | `rc=0` | `note=PASS`
- `zipgrep` | `cmd=echo test > /tmp/test.txt && zip /tmp/test.zip /tmp/test.txt && zipgrep test /tmp/test.zip` | `rc=0` | `note=PASS`
- `zipdetails` | `cmd=zipdetails /tmp/test.zip 2>/dev/null | head -5` | `rc=0` | `note=PASS`
- `pmap` | `cmd=pmap -x 1 2>/dev/null | head -5` | `rc=0` | `note=PASS`
- `gdbus` | `cmd=gdbus --version 2>&1 | head -1` | `rc=0` | `note=PASS`
- `hostnamectl` | `cmd=hostnamectl status 2>&1 | head -5` | `rc=0` | `note=PASS`

## Failures from on-device test run — 2026-09-10 21:07:26

Tested with `elf_loader_test.sh` (all ELF executables, modes: `--ownall`, `--shim`, `--lazy --ownall`, `--run`).

Total executables with at least one failure: 136

### `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/jar`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/javac`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/jdeprscan`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/jinfo`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/jlink`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/jpackage`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/jshell`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/jstatd`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/rmiregistry`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/serialver`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/glslc`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/spirv-as`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/spirv-cfg`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/spirv-dis`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/spirv-link`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/spirv-opt`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/spirv-reduce`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/spirv-val`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/libc++.so: not an E`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/19/lib/linux/aarch64/lldb-server`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00200000-006e1000 r--p 00000000 00:00 0  006e1000-006e4000 rw-p 00000000 00:00 0  006e4000-01a6c000 r-xp `
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00200000-006e1000 r--p 00000000 00:00 0  006e1000-006e4000 rw-p 00000000 00:00 0  006e4000-01a6c000 r-xp `
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00200000-006e1000 r--p 00000000 00:00 0  006e1000-006e4000 rw-p 00000000 00:00 0  006e4000-01a6c000 r-xp `
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00200000-006e1000 r--p 00000000 00:00 0  006e1000-006e4000 rw-p 00000000 00:00 0  006e4000-01a6c000 r-xp `
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00200000-006e1000 r--p 00000000 00:00 0  006e1000-006e4000 rw-p 00000000 00:00 0  006e4000-01a6c000 r-xp `
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00200000-006e1000 r--p 00000000 00:00 0  006e1000-006e4000 rw-p 00000000 00:00 0  006e4000-01a6c000 r-xp `
- `--ownall --version` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall --help` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall ` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim --version` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim --help` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim ` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-004c5000 r--p 00000000 00:00 0  004c5000-01693000 r-xp 00000000 00:00 0  01693000-01c17000 rw-p `
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-004c5000 r--p 00000000 00:00 0  004c5000-01693000 r-xp 00000000 00:00 0  01693000-01c17000 rw-p `
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-004c5000 r--p 00000000 00:00 0  004c5000-01693000 r-xp 00000000 00:00 0  01693000-01c17000 rw-p `
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-004c5000 r--p 00000000 00:00 0  004c5000-01693000 r-xp 00000000 00:00 0  01693000-01c17000 rw-p `
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-004c5000 r--p 00000000 00:00 0  004c5000-01693000 r-xp 00000000 00:00 0  01693000-01c17000 rw-p `
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-004c5000 r--p 00000000 00:00 0  004c5000-01693000 r-xp 00000000 00:00 0  01693000-01c17000 rw-p `
- `--ownall --version` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall --help` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall ` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim --version` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim --help` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim ` → **rc=1**  
  output: `[-] Only ELF64 supported`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/toolchains/llvm/prebuilt/linux-x86_64/python3/bin/python3.11`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00401000 r-xp 00000000 00:00 0  00401000-00602000 rw-p 00000000 00:00 0  56d3c61000-56d3c68000 r`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00401000 r-xp 00000000 00:00 0  00401000-00602000 rw-p 00000000 00:00 0  5f06803000-5f0680a000 r`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00401000 r-xp 00000000 00:00 0  00401000-00602000 rw-p 00000000 00:00 0  5c00a34000-5c00a3b000 r`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00401000 r-xp 00000000 00:00 0  00401000-00602000 rw-p 00000000 00:00 0  59ac4e6000-59ac4ed000 r`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00401000 r-xp 00000000 00:00 0  00401000-00602000 rw-p 00000000 00:00 0  55861f2000-55861f9000 r`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00401000 r-xp 00000000 00:00 0  00401000-00602000 rw-p 00000000 00:00 0  63ea930000-63ea937000 r`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.cache/copilot/pkg/linux-arm64/1.0.83/tgrep/bin/linux-arm64/tgrep`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5bf560c000-5bf5613000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 6123490000-6123497000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5df72e9000-5df72f0000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 640f5a7000-640f5ae000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 63b12d2000-63b12d9000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 590c738000-590c73f000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5fd4599000-5fd45a0000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5a2eaa1000-5a2eaa8000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 565a18a000-565a191000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5dcbe0c000-5dcbe13000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5d358db000-5d358e2000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5d20903000-5d2090a000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5d6ec7b000-5d6ec82000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 61efe79000-61efe80000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 640628b000-6406292000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5eea3f0000-5eea3f7000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5d91bce000-5d91bd5000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5b833db000-5b833e2000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.cache/uv/archive-v0/t2xQydCYlmEA5xh5/uv-0.12.9.data/scripts/uvx`

- `--ownall --version` → **rc=2**  
  output: `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
- `--ownall --help` → **rc=2**  
  output: `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
- `--ownall ` → **rc=2**  
  output: `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
- `--shim --version` → **rc=2**  
  output: `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
- `--shim --help` → **rc=2**  
  output: `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
- `--shim ` → **rc=2**  
  output: `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
- `--ownall --version` → **rc=2**  
  output: `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
- `--ownall --help` → **rc=2**  
  output: `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
- `--ownall ` → **rc=2**  
  output: `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
- `--shim --version` → **rc=2**  
  output: `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
- `--shim --help` → **rc=2**  
  output: `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
- `--shim ` → **rc=2**  
  output: `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.gradle/jdks/eclipse_adoptium-21-aarch64-linux.2/bin/jmod`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.gradle/jdks/eclipse_adoptium-21-aarch64-linux.2/bin/jrunscript`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.nvm/versions/node/v26.8.1/bin/node`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-0080a000 r--p 00000000 00:00 0  0080a000-00810000 rw-p 00000000 00:00 0  00810000-03188000 r-xp `
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-0080a000 r--p 00000000 00:00 0  0080a000-00810000 rw-p 00000000 00:00 0  00810000-03188000 r-xp `
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-0080a000 r--p 00000000 00:00 0  0080a000-00810000 rw-p 00000000 00:00 0  00810000-03188000 r-xp `
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-0080a000 r--p 00000000 00:00 0  0080a000-00810000 rw-p 00000000 00:00 0  00810000-03188000 r-xp `
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-0080a000 r--p 00000000 00:00 0  0080a000-00810000 rw-p 00000000 00:00 0  00810000-03188000 r-xp `
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-0080a000 r--p 00000000 00:00 0  0080a000-00810000 rw-p 00000000 00:00 0  00810000-03188000 r-xp `

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.nvm/versions/node/v26.8.1/lib/node_modules/@earendil-works/pi-coding-agent/node_modules/@esbuild/freebsd-x64/bin/esbuild`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-004b6000 r-xp 00000000 00:00 0  004b6000-004c0000 rw-p 00000000 00:00 0  004c0000-00965000 r--p `
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-004b6000 r-xp 00000000 00:00 0  004b6000-004c0000 rw-p 00000000 00:00 0  004c0000-00965000 r--p `
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-004b6000 r-xp 00000000 00:00 0  004b6000-004c0000 rw-p 00000000 00:00 0  004c0000-00965000 r--p `
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-004b6000 r-xp 00000000 00:00 0  004b6000-004c0000 rw-p 00000000 00:00 0  004c0000-00965000 r--p `
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-004b6000 r-xp 00000000 00:00 0  004b6000-004c0000 rw-p 00000000 00:00 0  004c0000-00965000 r--p `
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-004b6000 r-xp 00000000 00:00 0  004b6000-004c0000 rw-p 00000000 00:00 0  004c0000-00965000 r--p `
- `--ownall --version` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall --help` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall ` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim --version` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim --help` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim ` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall --version` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall --help` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall ` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim --version` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim --help` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim ` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-0099d000 r-xp 00000000 00:00 0  0099d000-00e69000 r--p 00000000 00:00 0  00e69000-02f05000 rw-p `
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-0099d000 r-xp 00000000 00:00 0  0099d000-00e69000 r--p 00000000 00:00 0  00e69000-02f05000 rw-p `
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-0099d000 r-xp 00000000 00:00 0  0099d000-00e69000 r--p 00000000 00:00 0  00e69000-02f05000 rw-p `
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-0099d000 r-xp 00000000 00:00 0  0099d000-00e69000 r--p 00000000 00:00 0  00e69000-02f05000 rw-p `
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-0099d000 r-xp 00000000 00:00 0  0099d000-00e69000 r--p 00000000 00:00 0  00e69000-02f05000 rw-p `
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-0099d000 r-xp 00000000 00:00 0  0099d000-00e69000 r--p 00000000 00:00 0  00e69000-02f05000 rw-p `
- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00580000 r-xp 00000000 00:00 0  00580000-00a40000 r--p 00000000 00:00 0  00a40000-02aed000 rw-p `
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00580000 r-xp 00000000 00:00 0  00580000-00a40000 r--p 00000000 00:00 0  00a40000-02aed000 rw-p `
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00580000 r-xp 00000000 00:00 0  00580000-00a40000 r--p 00000000 00:00 0  00a40000-02aed000 rw-p `
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00580000 r-xp 00000000 00:00 0  00580000-00a40000 r--p 00000000 00:00 0  00a40000-02aed000 rw-p `
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00580000 r-xp 00000000 00:00 0  00580000-00a40000 r--p 00000000 00:00 0  00a40000-02aed000 rw-p `
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00580000 r-xp 00000000 00:00 0  00580000-00a40000 r--p 00000000 00:00 0  00a40000-02aed000 rw-p `
- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00566000 r-xp 00000000 00:00 0  00566000-00570000 rw-p 00000000 00:00 0  00570000-00a10000 r--p `
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00566000 r-xp 00000000 00:00 0  00566000-00570000 rw-p 00000000 00:00 0  00570000-00a10000 r--p `
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00566000 r-xp 00000000 00:00 0  00566000-00570000 rw-p 00000000 00:00 0  00570000-00a10000 r--p `
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00566000 r-xp 00000000 00:00 0  00566000-00570000 rw-p 00000000 00:00 0  00570000-00a10000 r--p `
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00566000 r-xp 00000000 00:00 0  00566000-00570000 rw-p 00000000 00:00 0  00570000-00a10000 r--p `
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00566000 r-xp 00000000 00:00 0  00566000-00570000 rw-p 00000000 00:00 0  00570000-00a10000 r--p `
- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-006c1000 r-xp 00000000 00:00 0  006c1000-006d0000 rw-p 00000000 00:00 0  006d0000-00b5b000 r--p `
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-006c1000 r-xp 00000000 00:00 0  006c1000-006d0000 rw-p 00000000 00:00 0  006d0000-00b5b000 r--p `
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-006c1000 r-xp 00000000 00:00 0  006c1000-006d0000 rw-p 00000000 00:00 0  006d0000-00b5b000 r--p `
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-006c1000 r-xp 00000000 00:00 0  006c1000-006d0000 rw-p 00000000 00:00 0  006d0000-00b5b000 r--p `
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-006c1000 r-xp 00000000 00:00 0  006c1000-006d0000 rw-p 00000000 00:00 0  006d0000-00b5b000 r--p `
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-006c1000 r-xp 00000000 00:00 0  006c1000-006d0000 rw-p 00000000 00:00 0  006d0000-00b5b000 r--p `
- `--ownall --version` → **rc=1**  
  output: `[-] No LOAD segments`
- `--ownall --help` → **rc=1**  
  output: `[-] No LOAD segments`
- `--ownall ` → **rc=1**  
  output: `[-] No LOAD segments`
- `--shim --version` → **rc=1**  
  output: `[-] No LOAD segments`
- `--shim --help` → **rc=1**  
  output: `[-] No LOAD segments`
- `--shim ` → **rc=1**  
  output: `[-] No LOAD segments`
- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00480000 r-xp 00000000 00:00 0  00480000-00934000 r--p 00000000 00:00 0  00934000-029ed000 rw-p `
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00480000 r-xp 00000000 00:00 0  00480000-00934000 r--p 00000000 00:00 0  00934000-029ed000 rw-p `
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00480000 r-xp 00000000 00:00 0  00480000-00934000 r--p 00000000 00:00 0  00934000-029ed000 rw-p `
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00480000 r-xp 00000000 00:00 0  00480000-00934000 r--p 00000000 00:00 0  00934000-029ed000 rw-p `
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00480000 r-xp 00000000 00:00 0  00480000-00934000 r--p 00000000 00:00 0  00934000-029ed000 rw-p `
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00010000-00480000 r-xp 00000000 00:00 0  00480000-00934000 r--p 00000000 00:00 0  00934000-029ed000 rw-p `
- `--ownall --version` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=10193600`
- `--ownall --help` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=10193600`
- `--ownall ` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=10193600`
- `--shim --version` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=10193600`
- `--shim --help` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=10193600`
- `--shim ` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=10193600`
- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00999000 r-xp 00000000 00:00 0  00999000-00e60000 r--p 00000000 00:00 0  00e60000-02efb000 rw-p `
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00999000 r-xp 00000000 00:00 0  00999000-00e60000 r--p 00000000 00:00 0  00e60000-02efb000 rw-p `
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00999000 r-xp 00000000 00:00 0  00999000-00e60000 r--p 00000000 00:00 0  00e60000-02efb000 rw-p `
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00999000 r-xp 00000000 00:00 0  00999000-00e60000 r--p 00000000 00:00 0  00e60000-02efb000 rw-p `
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00999000 r-xp 00000000 00:00 0  00999000-00e60000 r--p 00000000 00:00 0  00e60000-02efb000 rw-p `
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00999000 r-xp 00000000 00:00 0  00999000-00e60000 r--p 00000000 00:00 0  00e60000-02efb000 rw-p `
- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object [-]`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object [-]`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object [-]`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object [-]`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object [-]`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object [-]`
- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-009a4000 r-xp 00000000 00:00 0  009a4000-00e76000 r--p 00000000 00:00 0  00e76000-02f23000 rw-p `
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-009a4000 r-xp 00000000 00:00 0  009a4000-00e76000 r--p 00000000 00:00 0  00e76000-02f23000 rw-p `
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-009a4000 r-xp 00000000 00:00 0  009a4000-00e76000 r--p 00000000 00:00 0  00e76000-02f23000 rw-p `
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-009a4000 r-xp 00000000 00:00 0  009a4000-00e76000 r--p 00000000 00:00 0  00e76000-02f23000 rw-p `
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-009a4000 r-xp 00000000 00:00 0  009a4000-00e76000 r--p 00000000 00:00 0  00e76000-02f23000 rw-p `
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-009a4000 r-xp 00000000 00:00 0  009a4000-00e76000 r--p 00000000 00:00 0  00e76000-02f23000 rw-p `
- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] dep libsendfile.so not found [-] dep libsocket.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/u`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] dep libsendfile.so not found [-] dep libsocket.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/u`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] dep libsendfile.so not found [-] dep libsocket.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/u`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] dep libsendfile.so not found [-] dep libsocket.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/u`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] dep libsendfile.so not found [-] dep libsocket.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/u`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] dep libsendfile.so not found [-] dep libsocket.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/u`
- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object [-]`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object [-]`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object [-]`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object [-]`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object [-]`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object [-]`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.nvm/versions/node/v26.8.1/lib/node_modules/@github/copilot/node_modules/@github/copilot-linux-arm64/copilot`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-064dd000 r-xp 00000000 00:00 0  064dd000-0e160000 rw-p 00000000 00:00 0  0e160000-116a0000 r--p `
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-064dd000 r-xp 00000000 00:00 0  064dd000-0e160000 rw-p 00000000 00:00 0  0e160000-116a0000 r--p `
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-064dd000 r-xp 00000000 00:00 0  064dd000-0e160000 rw-p 00000000 00:00 0  0e160000-116a0000 r--p `
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-064dd000 r-xp 00000000 00:00 0  064dd000-0e160000 rw-p 00000000 00:00 0  0e160000-116a0000 r--p `
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-064dd000 r-xp 00000000 00:00 0  064dd000-0e160000 rw-p 00000000 00:00 0  0e160000-116a0000 r--p `
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-064dd000 r-xp 00000000 00:00 0  064dd000-0e160000 rw-p 00000000 00:00 0  0e160000-116a0000 r--p `

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.nvm/versions/node/v26.8.1/lib/node_modules/@github/copilot/node_modules/@github/copilot-linux-arm64/prebuilds/linux-arm64/copilot-runtime`

- `--ownall --version` → **rc=1**  
  output: `copilot-runtime: unsupported argument '--version'`
- `--ownall --help` → **rc=1**  
  output: `copilot-runtime: unsupported argument '--help'`
- `--ownall ` → **rc=1**  
  output: `copilot-runtime: SDK server mode requires --server or --headless`
- `--shim --version` → **rc=1**  
  output: `copilot-runtime: unsupported argument '--version'`
- `--shim --help` → **rc=1**  
  output: `copilot-runtime: unsupported argument '--help'`
- `--shim ` → **rc=1**  
  output: `copilot-runtime: SDK server mode requires --server or --headless`
- `--ownall --version` → **rc=1**  
  output: `copilot-runtime: unsupported argument '--version'`
- `--ownall --help` → **rc=1**  
  output: `copilot-runtime: unsupported argument '--help'`
- `--ownall ` → **rc=1**  
  output: `copilot-runtime: SDK server mode requires --server or --headless`
- `--shim --version` → **rc=1**  
  output: `copilot-runtime: unsupported argument '--version'`
- `--shim --help` → **rc=1**  
  output: `copilot-runtime: unsupported argument '--help'`
- `--shim ` → **rc=1**  
  output: `copilot-runtime: SDK server mode requires --server or --headless`
- `--ownall --version` → **rc=1**  
  output: `copilot-runtime: unsupported argument '--version'`
- `--ownall --help` → **rc=1**  
  output: `copilot-runtime: unsupported argument '--help'`
- `--ownall ` → **rc=1**  
  output: `copilot-runtime: SDK server mode requires --server or --headless`
- `--shim --version` → **rc=1**  
  output: `copilot-runtime: unsupported argument '--version'`
- `--shim --help` → **rc=1**  
  output: `copilot-runtime: unsupported argument '--help'`
- `--shim ` → **rc=1**  
  output: `copilot-runtime: SDK server mode requires --server or --headless`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.nvm/versions/node/v26.8.1/lib/node_modules/@github/copilot/node_modules/@github/copilot-linux-arm64/ripgrep/bin/linux-arm64/rg`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 60f0682000-60f0689000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 58aeece000-58aeed5000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 62ab673000-62ab67a000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 56e8669000-56e8670000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 59de7f6000-59de7fd000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 6048c49000-6048c50000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 645b1f6000-645b1fd000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 59e3873000-59e387a000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 61757ac000-61757b3000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5d4792a000-5d47931000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 616113d000-6161144000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5e53d91000-5e53d98000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 64ce181000-64ce188000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 6421c09000-6421c10000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 57a3f0c000-57a3f13000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 60016fe000-6001705000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5c47361000-5c47368000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 58c01d6000-58c01dd000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 59a46e0000-59a46e7000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 63fe174000-63fe17b000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5d2b8da000-5d2b8e1000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 56b4f8b000-56b4f92000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 59ae8b0000-59ae8b7000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 60f5918000-60f591f000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.nvm/versions/node/v26.8.1/lib/node_modules/@kilocode/cli/node_modules/@kilocode/cli-linux-arm64/bin/.l2s.kilo0001.0002`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `============================================================ Bun v1.3.14 (0d9b296a) Linux arm64 Linux Kernel v4.14.190 |`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `============================================================ Bun v1.3.14 (0d9b296a) Linux arm64 Linux Kernel v4.14.190 |`
- `--ownall ` → **CRASH (SIG11)**  
  output: `============================================================ Bun v1.3.14 (0d9b296a) Linux arm64 Linux Kernel v4.14.190 |`
- `--shim --version` → **CRASH (SIG11)**  
  output: `============================================================ Bun v1.3.14 (0d9b296a) Linux arm64 Linux Kernel v4.14.190 |`
- `--shim --help` → **CRASH (SIG11)**  
  output: `============================================================ Bun v1.3.14 (0d9b296a) Linux arm64 Linux Kernel v4.14.190 |`
- `--shim ` → **CRASH (SIG11)**  
  output: `============================================================ Bun v1.3.14 (0d9b296a) Linux arm64 Linux Kernel v4.14.190 |`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.nvm/versions/node/v26.8.1/lib/node_modules/@kilocode/cli/node_modules/@kilocode/cli-linux-arm64/bin/kilo-sandbox-seccomp`

- `--ownall --version` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=99`
- `--ownall --help` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=99`
- `--ownall ` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=99`
- `--shim --version` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=99`
- `--shim --help` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=99`
- `--shim ` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=99`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/candidates/java/25.0.4-tem/bin/javadoc`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/candidates/java/25.0.4-tem/bin/jcmd`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/candidates/java/25.0.4-tem/bin/jimage`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/candidates/java/25.0.4-tem/bin/jnativescan`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/candidates/java/25.0.4-tem/bin/jps`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/candidates/java/25.0.4-tem/bin/jstack`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/candidates/java/25.0.4-tem/bin/jwebserver`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/libexec/current`

- `--ownall --version` → **rc=2**  
  output: `error: unexpected argument '--version' found    tip: to pass '--version' as a value, use '-- --version'  Usage: sdk curr`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/libexec/default`

- `--ownall --version` → **rc=2**  
  output: `error: unexpected argument '--version' found    tip: to pass '--version' as a value, use '-- --version'  Usage: sdk defa`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/libexec/help`

- `--ownall --version` → **rc=2**  
  output: `error: unexpected argument '--version' found  Usage: help [COMMAND]  For more information, try '--help'.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/libexec/home`

- `--ownall --version` → **rc=2**  
  output: `error: unexpected argument '--version' found    tip: to pass '--version' as a value, use '-- --version'  Usage: sdk home`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/libexec/uninstall`

- `--ownall --version` → **rc=2**  
  output: `error: unexpected argument '--version' found    tip: to pass '--version' as a value, use '-- --version'  Usage: sdk unin`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/libexec/version`

- `--ownall --version` → **CRASH (SIG11)**  
  output: ` thread 'main' (20726) panicked at src/lib.rs:38:13: not a valid path: /data/user/0/com.linux_core/files/.sdkman/var/ver`
- `--ownall --help` → **CRASH (SIG11)**  
  output: ` thread 'main' (20731) panicked at src/lib.rs:38:13: not a valid path: /data/user/0/com.linux_core/files/.sdkman/var/ver`
- `--ownall ` → **CRASH (SIG11)**  
  output: ` thread 'main' (20736) panicked at src/lib.rs:38:13: not a valid path: /data/user/0/com.linux_core/files/.sdkman/var/ver`
- `--shim --version` → **CRASH (SIG11)**  
  output: ` thread 'main' (20741) panicked at src/lib.rs:38:13: not a valid path: /data/user/0/com.linux_core/files/.sdkman/var/ver`
- `--shim --help` → **CRASH (SIG11)**  
  output: ` thread 'main' (20746) panicked at src/lib.rs:38:13: not a valid path: /data/user/0/com.linux_core/files/.sdkman/var/ver`
- `--shim ` → **CRASH (SIG11)**  
  output: ` thread 'main' (20751) panicked at src/lib.rs:38:13: not a valid path: /data/user/0/com.linux_core/files/.sdkman/var/ver`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/docs_config_memo/.gemini/antigravity-cli/bin/webm_encoder`

- `--ownall --version` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--ownall --help` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--ownall ` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--shim --version` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--shim --help` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--shim ` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/docs_config_memo/.local/share/gh/extensions/gh-models/gh-models`

- `--ownall --version` → **rc=1**  
  output: `No GitHub token found. Please run 'gh auth login' to authenticate. Error: unknown flag: --version Usage:   gh models [co`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/docs_config_memo/.local/share/gh/extensions/gh-s/gh-s`

- `--ownall --version` → **rc=1**  
  output: `gh-s 0.0.12`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/elf_loader/elf_loader`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/elf_loader/magisk-module/system/bin/gbsh`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/kali_combined/kali_core_emulator/app/src/main/jniLibs/x86_64/libloader.so`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00200000-00201000 r--p 00000000 00:00 0  00201000-2000000000 rw-p 00000000 00:00 0  2000000000-2000001000`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00200000-00201000 r--p 00000000 00:00 0  00201000-2000000000 rw-p 00000000 00:00 0  2000000000-2000001000`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00200000-00201000 r--p 00000000 00:00 0  00201000-2000000000 rw-p 00000000 00:00 0  2000000000-2000001000`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00200000-00201000 r--p 00000000 00:00 0  00201000-2000000000 rw-p 00000000 00:00 0  2000000000-2000001000`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00200000-00201000 r--p 00000000 00:00 0  00201000-2000000000 rw-p 00000000 00:00 0  2000000000-2000001000`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00200000-00201000 r--p 00000000 00:00 0  00201000-2000000000 rw-p 00000000 00:00 0  2000000000-2000001000`
- `--ownall --version` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall --help` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall ` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim --version` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim --help` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim ` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall --version` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall --help` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall ` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim --version` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim --help` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim ` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall --version` → **rc=1**  
  output: `[-] No LOAD segments`
- `--ownall --help` → **rc=1**  
  output: `[-] No LOAD segments`
- `--ownall ` → **rc=1**  
  output: `[-] No LOAD segments`
- `--shim --version` → **rc=1**  
  output: `[-] No LOAD segments`
- `--shim --help` → **rc=1**  
  output: `[-] No LOAD segments`
- `--shim ` → **rc=1**  
  output: `[-] No LOAD segments`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/root/kali_combined/kali_core_emulator/app/src/main/jniLibs/x86_64/libproot.so`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 61614fd000-6161504000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5bb3b2b000-5bb3b32000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 59597aa000-59597b1000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 64a9947000-64a994e000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 642d651000-642d658000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5fd2424000-5fd242b000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --version` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall --help` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall ` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim --version` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim --help` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim ` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall --version` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall --help` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall ` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim --version` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim --help` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--shim ` → **rc=1**  
  output: `[-] Only ELF64 supported`
- `--ownall --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object   [`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object   [`
- `--ownall ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object   [`
- `--shim --version` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object   [`
- `--shim --help` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object   [`
- `--shim ` → **CRASH (SIG11)**  
  output: `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so: not an ELF64 shared object   [`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/aarch64-linux-gnu-gcc-14`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00506000 r-xp 00000000 00:00 0  00506000-00525000 rw-p 00000000 00:00 0  619b0ef000-619b0f6000 r`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00506000 r-xp 00000000 00:00 0  00506000-00525000 rw-p 00000000 00:00 0  588aea4000-588aeab000 r`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00506000 r-xp 00000000 00:00 0  00506000-00525000 rw-p 00000000 00:00 0  5d69c9b000-5d69ca2000 r`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00506000 r-xp 00000000 00:00 0  00506000-00525000 rw-p 00000000 00:00 0  62f2963000-62f296a000 r`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00506000 r-xp 00000000 00:00 0  00506000-00525000 rw-p 00000000 00:00 0  60299e8000-60299ef000 r`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00506000 r-xp 00000000 00:00 0  00506000-00525000 rw-p 00000000 00:00 0  6133f7a000-6133f81000 r`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/aarch64-linux-gnu-gcc-nm-14`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  651003a000-6510041000 r`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  62281fa000-6228201000 r`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  62f9c82000-62f9c89000 r`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  6350abd000-6350ac4000 r`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  56352c0000-56352c7000 r`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  59e4faa000-59e4fb1000 r`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/aarch64-linux-gnu-gcc-ranlib-14`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  5974bcd000-5974bd4000 r`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  5c9eb2b000-5c9eb32000 r`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  595245d000-5952464000 r`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  5a731a6000-5a731ad000 r`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  627b9bd000-627b9c4000 r`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  62b130b000-62b1312000 r`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/aarch64-linux-gnu-gcov-14`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00471000 r-xp 00000000 00:00 0  00471000-00491000 rw-p 00000000 00:00 0  558fb09000-558fb10000 r`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00471000 r-xp 00000000 00:00 0  00471000-00491000 rw-p 00000000 00:00 0  627a82d000-627a834000 r`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00471000 r-xp 00000000 00:00 0  00471000-00491000 rw-p 00000000 00:00 0  5b64033000-5b6403a000 r`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00471000 r-xp 00000000 00:00 0  00471000-00491000 rw-p 00000000 00:00 0  5664555000-566455c000 r`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00471000 r-xp 00000000 00:00 0  00471000-00491000 rw-p 00000000 00:00 0  63a1a17000-63a1a1e000 r`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00471000 r-xp 00000000 00:00 0  00471000-00491000 rw-p 00000000 00:00 0  61d72f3000-61d72fa000 r`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/aarch64-linux-gnu-gcov-dump-14`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00459000 r-xp 00000000 00:00 0  00459000-00471000 rw-p 00000000 00:00 0  614b65f000-614b666000 r`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00459000 r-xp 00000000 00:00 0  00459000-00471000 rw-p 00000000 00:00 0  5bb3926000-5bb392d000 r`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00459000 r-xp 00000000 00:00 0  00459000-00471000 rw-p 00000000 00:00 0  5f89b01000-5f89b08000 r`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00459000 r-xp 00000000 00:00 0  00459000-00471000 rw-p 00000000 00:00 0  5a50c62000-5a50c69000 r`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00459000 r-xp 00000000 00:00 0  00459000-00471000 rw-p 00000000 00:00 0  5a21b4a000-5a21b51000 r`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00459000 r-xp 00000000 00:00 0  00459000-00471000 rw-p 00000000 00:00 0  59e4015000-59e401c000 r`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/aarch64-linux-gnu-gcov-tool-14`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00461000 r-xp 00000000 00:00 0  00461000-00481000 rw-p 00000000 00:00 0  565bdbf000-565bdc6000 r`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00461000 r-xp 00000000 00:00 0  00461000-00481000 rw-p 00000000 00:00 0  63a940a000-63a9411000 r`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00461000 r-xp 00000000 00:00 0  00461000-00481000 rw-p 00000000 00:00 0  55ecd7a000-55ecd81000 r`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00461000 r-xp 00000000 00:00 0  00461000-00481000 rw-p 00000000 00:00 0  55afcc0000-55afcc7000 r`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00461000 r-xp 00000000 00:00 0  00461000-00481000 rw-p 00000000 00:00 0  5555808000-555580f000 r`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-00461000 r-xp 00000000 00:00 0  00461000-00481000 rw-p 00000000 00:00 0  620d2c3000-620d2ca000 r`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/aarch64-linux-gnu-lto-dump-14`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-01fd0000 r-xp 00000000 00:00 0  01fd0000-02201000 rw-p 00000000 00:00 0  558d4f7000-558d4fe000 r`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-01fd0000 r-xp 00000000 00:00 0  01fd0000-02201000 rw-p 00000000 00:00 0  5f1dfd0000-5f1dfd7000 r`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-01fd0000 r-xp 00000000 00:00 0  01fd0000-02201000 rw-p 00000000 00:00 0  5a368e9000-5a368f0000 r`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-01fd0000 r-xp 00000000 00:00 0  01fd0000-02201000 rw-p 00000000 00:00 0  5d07ccc000-5d07cd3000 r`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-01fd0000 r-xp 00000000 00:00 0  01fd0000-02201000 rw-p 00000000 00:00 0  5d02a16000-5d02a1d000 r`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 00400000-01fd0000 r-xp 00000000 00:00 0  01fd0000-02201000 rw-p 00000000 00:00 0  559d60e000-559d615000 r`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/busybox`

- `--ownall --version` → **rc=127**  
  output: `--version: applet not found`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/catman`

- `--ownall --version` → **rc=64**  
  output: `catman: can't set the locale; make sure $LC_* and $LANG are correct /data/user/0/com.linux_core/files/nh/distro/parrot/u`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chacl`

- `--ownall --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chacl: invalid option -- '-' Usage: 	chacl acl pathname... 	c`
- `--ownall --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chacl: invalid option -- '-' Usage: 	chacl acl pathname... 	c`
- `--ownall ` → **rc=1**  
  output: `Usage: 	chacl acl pathname... 	chacl -b acl dacl pathname... 	chacl -d dacl pathname... 	chacl -R pathname... 	chacl -D `
- `--shim --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chacl: invalid option -- '-' Usage: 	chacl acl pathname... 	c`
- `--shim --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chacl: invalid option -- '-' Usage: 	chacl acl pathname... 	c`
- `--shim ` → **rc=1**  
  output: `Usage: 	chacl acl pathname... 	chacl -b acl dacl pathname... 	chacl -d dacl pathname... 	chacl -R pathname... 	chacl -D `

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chage`

- `--ownall --version` → **rc=1**  
  output: `Cannot open audit interface - aborting.`
- `--ownall --help` → **rc=1**  
  output: `Cannot open audit interface - aborting.`
- `--ownall ` → **rc=1**  
  output: `Cannot open audit interface - aborting.`
- `--shim --version` → **rc=1**  
  output: `Cannot open audit interface - aborting.`
- `--shim --help` → **rc=1**  
  output: `Cannot open audit interface - aborting.`
- `--shim ` → **rc=1**  
  output: `Cannot open audit interface - aborting.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chattr`

- `--ownall --version` → **rc=1**  
  output: `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chattr [-RVf] [-+=aAcCdDeijPsStTuFx] [-p project] [-v `
- `--ownall --help` → **rc=1**  
  output: `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chattr [-RVf] [-+=aAcCdDeijPsStTuFx] [-p project] [-v `
- `--ownall ` → **rc=1**  
  output: `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chattr [-RVf] [-+=aAcCdDeijPsStTuFx] [-p project] [-v `
- `--shim --version` → **rc=1**  
  output: `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chattr [-RVf] [-+=aAcCdDeijPsStTuFx] [-p project] [-v `
- `--shim --help` → **rc=1**  
  output: `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chattr [-RVf] [-+=aAcCdDeijPsStTuFx] [-p project] [-v `
- `--shim ` → **rc=1**  
  output: `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chattr [-RVf] [-+=aAcCdDeijPsStTuFx] [-p project] [-v `

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-monitor`

- `--ownall --version` → **rc=1**  
  output: `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-monitor [--system | --session | --address ADDRESS`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-send`

- `--ownall --version` → **rc=1**  
  output: `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-send [--help] [--system | --session | --bus=ADDRE`
- `--ownall --help` → **rc=1**  
  output: `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-send [--help] [--system | --session | --bus=ADDRE`
- `--ownall ` → **rc=1**  
  output: `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-send [--help] [--system | --session | --bus=ADDRE`
- `--shim --version` → **rc=1**  
  output: `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-send [--help] [--system | --session | --bus=ADDRE`
- `--shim --help` → **rc=1**  
  output: `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-send [--help] [--system | --session | --bus=ADDRE`
- `--shim ` → **rc=1**  
  output: `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-send [--help] [--system | --session | --bus=ADDRE`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-update-activation-environment`

- `--ownall --version` → **rc=64**  
  output: `dbus-update-activation-environment: update environment variables that will be set for D-Bus     session services  dbus-u`
- `--ownall --help` → **rc=64**  
  output: `dbus-update-activation-environment: update environment variables that will be set for D-Bus     session services  dbus-u`
- `--ownall ` → **rc=71**  
  output: `dbus-update-activation-environment: error: unable to connect to D-Bus: Unable to autolaunch a dbus-daemon without a $DIS`
- `--shim --version` → **rc=64**  
  output: `dbus-update-activation-environment: update environment variables that will be set for D-Bus     session services  dbus-u`
- `--shim --help` → **rc=64**  
  output: `dbus-update-activation-environment: update environment variables that will be set for D-Bus     session services  dbus-u`
- `--shim ` → **rc=71**  
  output: `dbus-update-activation-environment: error: unable to connect to D-Bus: Unable to autolaunch a dbus-daemon without a $DIS`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearconvert`

- `--ownall --version` → **rc=1**  
  output: `All arguments must be specified Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearconvert <input`
- `--ownall --help` → **rc=1**  
  output: `All arguments must be specified Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearconvert <input`
- `--ownall ` → **rc=1**  
  output: `All arguments must be specified Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearconvert <input`
- `--shim --version` → **rc=1**  
  output: `All arguments must be specified Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearconvert <input`
- `--shim --help` → **rc=1**  
  output: `All arguments must be specified Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearconvert <input`
- `--shim ` → **rc=1**  
  output: `All arguments must be specified Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearconvert <input`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearkey`

- `--ownall --version` → **rc=1**  
  output: `Unknown argument --version Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearkey -t <type> -f <f`
- `--ownall --help` → **rc=1**  
  output: `Unknown argument --help Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearkey -t <type> -f <file`
- `--ownall ` → **rc=1**  
  output: `Must specify a key filename Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearkey -t <type> -f <`
- `--shim --version` → **rc=1**  
  output: `Unknown argument --version Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearkey -t <type> -f <f`
- `--shim --help` → **rc=1**  
  output: `Unknown argument --help Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearkey -t <type> -f <file`
- `--shim ` → **rc=1**  
  output: `Must specify a key filename Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearkey -t <type> -f <`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/funzip`

- `--ownall --version` → **rc=3**  
  output: `funzip error: input not a zip or gzip file`
- `--ownall --help` → **rc=3**  
  output: `funzip error: input not a zip or gzip file`
- `--ownall ` → **rc=3**  
  output: `funzip error: input not a zip or gzip file`
- `--shim --version` → **rc=3**  
  output: `funzip error: input not a zip or gzip file`
- `--shim --help` → **rc=3**  
  output: `funzip error: input not a zip or gzip file`
- `--shim ` → **rc=3**  
  output: `funzip error: input not a zip or gzip file`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/fzf`

- `--ownall --version` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--ownall --help` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--ownall ` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--shim --version` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--shim --help` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--shim ` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/gencat`

- `--ownall --version` → **rc=64**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/gencat: unrecognized option '--version' Try `gencat --help' o`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/getent`

- `--ownall --version` → **rc=64**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/getent: unrecognized option '--version' Try `getent --help' o`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/gh`

- `--ownall --version` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--ownall --help` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--ownall ` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--shim --version` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--shim --help` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--shim ` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/git-lfs`

- `--ownall --version` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--ownall --help` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--ownall ` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--shim --version` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--shim --help` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `--shim ` → **CRASH (SIG6)**  
  output: `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/gpgparsemail`

- `--ownall --version` → **rc=1**  
  output: `gpgparsemail: can't open '--version': No such file or directory`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/groff`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `GNU groff version 1.23.0 Copyright (C) 2022 Free Software Foundation, Inc. GNU groff comes with ABSOLUTELY NO WARRANTY. `

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/html2text`

- `--ownall --version` → **rc=1**  
  output: `Unrecognized command line option --version"`
- `--ownall --help` → **rc=1**  
  output: `Unrecognized command line option --help"`
- `--ownall ` → **rc=1**  
  output: `Opening input file -": invalid from_encoding"`
- `--shim --version` → **rc=1**  
  output: `Unrecognized command line option --version"`
- `--shim --help` → **rc=1**  
  output: `Unrecognized command line option --help"`
- `--shim ` → **rc=1**  
  output: `Opening input file -": invalid from_encoding"`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/iconv`

- `--ownall --version` → **rc=64**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/iconv: unrecognized option '--version' Try `iconv --help' or `

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/infocmp`

- `--ownall --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/infocmp: invalid option -- '-' Usage: infocmp [options] [-A d`
- `--ownall --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/infocmp: invalid option -- '-' Usage: infocmp [options] [-A d`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/jarsigner`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/java`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/javap`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/jconsole`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/jexec`

- `--ownall --version` → **rc=1**  
  output: `invalid path: No such file or directory`
- `--ownall --help` → **rc=1**  
  output: `invalid path: No such file or directory`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=1**  
  output: `invalid path: No such file or directory`
- `--shim --help` → **rc=1**  
  output: `invalid path: No such file or directory`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=1**  
  output: `invalid path: No such file or directory`
- `--ownall --help` → **rc=1**  
  output: `invalid path: No such file or directory`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=1**  
  output: `invalid path: No such file or directory`
- `--shim --help` → **rc=1**  
  output: `invalid path: No such file or directory`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/jfr`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/jmap`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/keytool`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/lexgrog`

- `--ownall --version` → **rc=64**  
  output: `lexgrog: can't set the locale; make sure $LC_* and $LANG are correct /data/user/0/com.linux_core/files/nh/distro/parrot/`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/locale`

- `--ownall --version` → **rc=64**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/locale: Cannot set LC_CTYPE to default locale: No such file o`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/localedef`

- `--ownall --version` → **rc=4**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/localedef: unrecognized option '--version' Try `localedef --h`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/lsattr`

- `--ownall --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/lsattr: invalid option -- '-' Usage: /data/user/0/com.linux_c`
- `--ownall --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/lsattr: invalid option -- '-' Usage: /data/user/0/com.linux_c`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/lsof`

- `--ownall --version` → **rc=1**  
  output: `lsof: illegal option character: - lsof: -e not followed by a file system path: rsion" lsof 4.99.4  latest revision: http`
- `--ownall --help` → **rc=1**  
  output: `lsof: illegal option character: - lsof: -e not followed by a file system path: lp" lsof 4.99.4  latest revision: https:/`
- `--ownall ` → **TIMEOUT**  
- `--shim --version` → **rc=1**  
  output: `lsof: illegal option character: - lsof: -e not followed by a file system path: rsion" lsof 4.99.4  latest revision: http`
- `--shim --help` → **rc=1**  
  output: `lsof: illegal option character: - lsof: -e not followed by a file system path: lp" lsof 4.99.4  latest revision: https:/`
- `--shim ` → **TIMEOUT**  

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/luit`

- `--ownall --version` → **rc=1**  
  output: `Warning: couldn't set locale. Unknown option --version /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/luit  `
- `--ownall --help` → **rc=1**  
  output: `Warning: couldn't set locale. Unknown option --help /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/luit   [ `
- `--ownall ` → **CRASH (SIG11)**  
  output: `Warning: couldn't set locale. Warning: couldn't find charset data for locale C.UTF-8; using ISO 8859-1. Warning: could n`
- `--shim --version` → **rc=1**  
  output: `Warning: couldn't set locale. Unknown option --version /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/luit  `
- `--shim --help` → **rc=1**  
  output: `Warning: couldn't set locale. Unknown option --help /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/luit   [ `
- `--shim ` → **CRASH (SIG11)**  
  output: `Warning: couldn't set locale. Warning: couldn't find charset data for locale C.UTF-8; using ISO 8859-1. Warning: could n`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/man`

- `--ownall --version` → **rc=64**  
  output: `man: can't set the locale; make sure $LC_* and $LANG are correct /data/user/0/com.linux_core/files/nh/distro/parrot/usr/`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/man-recode`

- `--ownall --version` → **rc=64**  
  output: `man-recode: can't set the locale; make sure $LC_* and $LANG are correct /data/user/0/com.linux_core/files/nh/distro/parr`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5cd179d000-5cd17a4000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall ` → **rc=64**  
  output: `man-recode: can't set the locale; make sure $LC_* and $LANG are correct Usage: man-recode [OPTION...]             -t COD`
- `--shim --version` → **rc=64**  
  output: `man-recode: can't set the locale; make sure $LC_* and $LANG are correct /data/user/0/com.linux_core/files/nh/distro/parr`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/mandb`

- `--ownall --version` → **rc=64**  
  output: `mandb: can't set the locale; make sure $LC_* and $LANG are correct /data/user/0/com.linux_core/files/nh/distro/parrot/us`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/manpath`

- `--ownall --version` → **rc=64**  
  output: `manpath: can't set the locale; make sure $LC_* and $LANG are correct /data/user/0/com.linux_core/files/nh/distro/parrot/`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/peekfd`

- `--ownall --version` → **rc=1**  
  output: `peekfd (PSmisc) 23.7 Copyright (C) 2007 Trent Waddington  PSmisc comes with ABSOLUTELY NO WARRANTY. This is free softwar`
- `--ownall --help` → **rc=1**  
  output: `Usage: peekfd [-8] [-n] [-c] [-d] [-V] [-h] <pid> [<fd> ..]     -8, --eight-bit-clean        output 8 bit clean streams.`
- `--ownall ` → **rc=1**  
  output: `Usage: peekfd [-8] [-n] [-c] [-d] [-V] [-h] <pid> [<fd> ..]     -8, --eight-bit-clean        output 8 bit clean streams.`
- `--shim --version` → **rc=1**  
  output: `peekfd (PSmisc) 23.7 Copyright (C) 2007 Trent Waddington  PSmisc comes with ABSOLUTELY NO WARRANTY. This is free softwar`
- `--shim --help` → **rc=1**  
  output: `Usage: peekfd [-8] [-n] [-c] [-d] [-V] [-h] <pid> [<fd> ..]     -8, --eight-bit-clean        output 8 bit clean streams.`
- `--shim ` → **rc=1**  
  output: `Usage: peekfd [-8] [-n] [-c] [-d] [-V] [-h] <pid> [<fd> ..]     -8, --eight-bit-clean        output 8 bit clean streams.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/pldd`

- `--ownall --version` → **rc=64**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/pldd: unrecognized option '--version' Try `pldd --help' or `p`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ps`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `Signal 11 (SEGV) caught by ps (4.0.4). /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ps:src/ps/display.c:75`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `Signal 11 (SEGV) caught by ps (4.0.4). /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ps:src/ps/display.c:75`
- `--ownall ` → **CRASH (SIG11)**  
  output: `Signal 11 (SEGV) caught by ps (4.0.4). /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ps:src/ps/display.c:75`
- `--shim --version` → **CRASH (SIG11)**  
  output: `Signal 11 (SEGV) caught by ps (4.0.4). /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ps:src/ps/display.c:75`
- `--shim --help` → **CRASH (SIG11)**  
  output: `Signal 11 (SEGV) caught by ps (4.0.4). /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ps:src/ps/display.c:75`
- `--shim ` → **CRASH (SIG11)**  
  output: `Signal 11 (SEGV) caught by ps (4.0.4). /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ps:src/ps/display.c:75`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/qemu-aarch64-static`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 60000000-60001000 r--p 00000000 00:00 0  60001000-6031d000 r-xp 00000000 00:00 0  6031d000-605b3000 r--p `
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 60000000-60001000 r--p 00000000 00:00 0  60001000-6031d000 r-xp 00000000 00:00 0  6031d000-605b3000 r--p `
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 60000000-60001000 r--p 00000000 00:00 0  60001000-6031d000 r-xp 00000000 00:00 0  6031d000-605b3000 r--p `
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 60000000-60001000 r--p 00000000 00:00 0  60001000-6031d000 r-xp 00000000 00:00 0  6031d000-605b3000 r--p `
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 60000000-60001000 r--p 00000000 00:00 0  60001000-6031d000 r-xp 00000000 00:00 0  6031d000-605b3000 r--p `
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 60000000-60001000 r--p 00000000 00:00 0  60001000-6031d000 r-xp 00000000 00:00 0  6031d000-605b3000 r--p `

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/scp`

- `--ownall --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/scp: unknown option -- -  usage: scp [-346ABCOpqRrsTv] [-c ci`
- `--ownall --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/scp: unknown option -- -  usage: scp [-346ABCOpqRrsTv] [-c ci`
- `--ownall ` → **CRASH (SIG127)**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/scp: unknown user 10315 `
- `--shim --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/scp: unknown option -- -  usage: scp [-346ABCOpqRrsTv] [-c ci`
- `--shim --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/scp: unknown option -- -  usage: scp [-346ABCOpqRrsTv] [-c ci`
- `--shim ` → **CRASH (SIG127)**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/scp: unknown user 10315 `

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/sftp`

- `--ownall --version` → **rc=1**  
  output: `unknown option -- -  usage: sftp [-46AaCfNpqrv] [-B buffer_size] [-b batchfile] [-c cipher]           [-D sftp_server_co`
- `--ownall --help` → **rc=1**  
  output: `unknown option -- -  usage: sftp [-46AaCfNpqrv] [-B buffer_size] [-b batchfile] [-c cipher]           [-D sftp_server_co`
- `--ownall ` → **rc=1**  
  output: `usage: sftp [-46AaCfNpqrv] [-B buffer_size] [-b batchfile] [-c cipher]           [-D sftp_server_command] [-F ssh_config`
- `--shim --version` → **rc=1**  
  output: `unknown option -- -  usage: sftp [-46AaCfNpqrv] [-B buffer_size] [-b batchfile] [-c cipher]           [-D sftp_server_co`
- `--shim --help` → **rc=1**  
  output: `unknown option -- -  usage: sftp [-46AaCfNpqrv] [-B buffer_size] [-b batchfile] [-c cipher]           [-D sftp_server_co`
- `--shim ` → **rc=1**  
  output: `usage: sftp [-46AaCfNpqrv] [-B buffer_size] [-b batchfile] [-c cipher]           [-D sftp_server_command] [-F ssh_config`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ssh-add`

- `--ownall --version` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `
- `--ownall --help` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `
- `--ownall ` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `
- `--shim --version` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `
- `--shim --help` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `
- `--shim ` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ssh-agent`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 582bde3000-582bdea000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 65188b8000-65188bf000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5b7f80c000-5b7f813000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5ee75c5000-5ee75cc000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 627877a000-6278781000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5fa15fd000-5fa1604000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ssh-keygen`

- `--ownall --version` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `
- `--ownall --help` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `
- `--ownall ` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `
- `--shim --version` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `
- `--shim --help` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `
- `--shim ` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ssh-keyscan`

- `--ownall --version` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `
- `--ownall --help` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `
- `--ownall ` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `
- `--shim --version` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `
- `--shim --help` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `
- `--shim ` → **CRASH (SIG127)**  
  output: `PRNG is not seeded `

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/starship`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5f30bb8000-5f30bbf000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5ce7441000-5ce7448000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5ab3df1000-5ab3df8000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 588f596000-588f59d000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 63dfc95000-63dfc9c000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 62df571000-62df578000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/sudo.orig`

- `--ownall --version` → **rc=1**  
  output: `sudo: /etc/sudo.conf is owned by uid 10315, should be 0 sudo: The no new privileges" flag is set`
- `--ownall --help` → **rc=1**  
  output: `sudo: /etc/sudo.conf is owned by uid 10315, should be 0 sudo: The no new privileges" flag is set`
- `--ownall ` → **rc=1**  
  output: `sudo: /etc/sudo.conf is owned by uid 10315, should be 0 sudo: The no new privileges" flag is set`
- `--shim --version` → **rc=1**  
  output: `sudo: /etc/sudo.conf is owned by uid 10315, should be 0 sudo: The no new privileges" flag is set`
- `--shim --help` → **rc=1**  
  output: `sudo: /etc/sudo.conf is owned by uid 10315, should be 0 sudo: The no new privileges" flag is set`
- `--shim ` → **rc=1**  
  output: `sudo: /etc/sudo.conf is owned by uid 10315, should be 0 sudo: The no new privileges" flag is set`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tabs`

- `--ownall --version` → **rc=1**  
  output: `Usage: tabs [options] [tabstop-list]  Options:   -0       reset tabs   -8       set tabs to standard interval   -a      `
- `--ownall --help` → **rc=1**  
  output: `Usage: tabs [options] [tabstop-list]  Options:   -0       reset tabs   -8       set tabs to standard interval   -a      `

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tic`

- `--ownall --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tic: invalid option -- '-' Usage: tic [-e names] [-o dir] [-R`
- `--ownall --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tic: invalid option -- '-' Usage: tic [-e names] [-o dir] [-R`
- `--ownall ` → **rc=1**  
  output: `tic: File name needed.  Usage: 	tic [-e names] [-o dir] [-R name] [-v[n]] [-V] [-w[n]] [-1aCDcfGgIKLNrsTtUx] source-file`
- `--shim --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tic: invalid option -- '-' Usage: tic [-e names] [-o dir] [-R`
- `--shim --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tic: invalid option -- '-' Usage: tic [-e names] [-o dir] [-R`
- `--shim ` → **rc=1**  
  output: `tic: File name needed.  Usage: 	tic [-e names] [-o dir] [-R name] [-v[n]] [-V] [-w[n]] [-1aCDcfGgIKLNrsTtUx] source-file`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/toe`

- `--ownall --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/toe: invalid option -- '-' usage: toe [-ahsuUV] [-v n] [file.`
- `--ownall --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/toe: invalid option -- '-' usage: toe [-ahsuUV] [-v n] [file.`
- `--shim --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/toe: invalid option -- '-' usage: toe [-ahsuUV] [-v n] [file.`
- `--shim --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/toe: invalid option -- '-' usage: toe [-ahsuUV] [-v n] [file.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tput`

- `--ownall --version` → **rc=2**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tput: invalid option -- '-' Usage: tput [options] [command]  `
- `--ownall --help` → **rc=2**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tput: invalid option -- '-' Usage: tput [options] [command]  `
- `--ownall ` → **rc=2**  
  output: `Usage: tput [options] [command]  Options:   -S <<       read commands from standard input   -T TERM     use this instead`
- `--shim --version` → **rc=2**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tput: invalid option -- '-' Usage: tput [options] [command]  `
- `--shim --help` → **rc=2**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tput: invalid option -- '-' Usage: tput [options] [command]  `
- `--shim ` → **rc=2**  
  output: `Usage: tput [options] [command]  Options:   -S <<       read commands from standard input   -T TERM     use this instead`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/udisksctl`

- `--ownall --version` → **CRASH (SIG11)**  
  output: ` (process:20798): GLib-CRITICAL **: 19:35:39.145: Failed to get RW lock 0x736be9ba30: Resource deadlock avoided  (proces`
- `--ownall --help` → **CRASH (SIG11)**  
  output: ` (process:20803): GLib-CRITICAL **: 19:35:39.516: Failed to get RW lock 0x769c961a30: Resource deadlock avoided  (proces`
- `--ownall ` → **rc=1**  
  output: ` (process:20808): GLib-CRITICAL **: 19:35:39.891: Failed to get RW lock 0x7ac792ba30: Resource deadlock avoided  (proces`
- `--shim --version` → **CRASH (SIG11)**  
  output: ` (process:20817): GLib-CRITICAL **: 19:35:40.269: Failed to get RW lock 0x719fc10a30: Resource deadlock avoided  (proces`
- `--shim --help` → **CRASH (SIG11)**  
  output: ` (process:20822): GLib-CRITICAL **: 19:35:40.632: Failed to get RW lock 0x6f565e5a30: Resource deadlock avoided  (proces`
- `--shim ` → **rc=1**  
  output: ` (process:20827): GLib-CRITICAL **: 19:35:41.004: Failed to get RW lock 0x7ea0582a30: Resource deadlock avoided  (proces`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/update-mime-database`

- `--ownall --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/update-mime-database: invalid option -- '-' Usage: /data/user`
- `--ownall --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/update-mime-database: invalid option -- '-' Usage: /data/user`
- `--ownall ` → **rc=1**  
  output: `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/update-mime-database [-hvVn] MIME-DIR`
- `--shim --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/update-mime-database: invalid option -- '-' Usage: /data/user`
- `--shim --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/update-mime-database: invalid option -- '-' Usage: /data/user`
- `--shim ` → **rc=1**  
  output: `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/update-mime-database [-hvVn] MIME-DIR`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/viewres`

- `--ownall --version` → **rc=1**  
  output: `Error: Can't open display: `
- `--ownall --help` → **rc=1**  
  output: `Error: Can't open display: `
- `--ownall ` → **rc=1**  
  output: `Error: Can't open display: `
- `--shim --version` → **rc=1**  
  output: `Error: Can't open display: `
- `--shim --help` → **rc=1**  
  output: `Error: Can't open display: `
- `--shim ` → **rc=1**  
  output: `Error: Can't open display: `

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/whatis`

- `--ownall --version` → **rc=64**  
  output: `whatis: can't set the locale; make sure $LC_* and $LANG are correct /data/user/0/com.linux_core/files/nh/distro/parrot/u`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/wmdocker`

- `--ownall --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/wmdocker - version 1.5 Copyright 2003, Ben Jansens <ben@orodu`
- `--ownall --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/wmdocker - version 1.5 Copyright 2003, Ben Jansens <ben@orodu`
- `--ownall ` → **CRASH (SIG11)**  
  output: `Segmentation Fault or Critical Error encountered. Dumping core and aborting.`
- `--shim --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/wmdocker - version 1.5 Copyright 2003, Ben Jansens <ben@orodu`
- `--shim --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/wmdocker - version 1.5 Copyright 2003, Ben Jansens <ben@orodu`
- `--shim ` → **CRASH (SIG11)**  
  output: `Segmentation Fault or Critical Error encountered. Dumping core and aborting.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdg-user-dirs-update`

- `--ownall --version` → **rc=1**  
  output: `Invalid argument --version`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdpyinfo`

- `--ownall --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdpyinfo: unrecognized argument '--version' usage:  /data/use`
- `--ownall --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdpyinfo: unrecognized argument '--help' usage:  /data/user/0`
- `--ownall ` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdpyinfo:  unable to open display ".`
- `--shim --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdpyinfo: unrecognized argument '--version' usage:  /data/use`
- `--shim --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdpyinfo: unrecognized argument '--help' usage:  /data/user/0`
- `--shim ` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdpyinfo:  unable to open display ".`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdriinfo`

- `--ownall --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 57ca187000-57ca18e000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5e10613000-5e1061a000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--ownall ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 577b922000-577b929000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --version` → **CRASH (SIG11)**  
  output: `  [maps-begin] 64bc112000-64bc119000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim --help` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5f54a88000-5f54a8f000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`
- `--shim ` → **CRASH (SIG11)**  
  output: `  [maps-begin] 5cd467b000-5cd4682000 r--p 00000000 fd:29 1236062                        /data/user/0/com.linux_core/file`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xev`

- `--ownall --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xev: warning: could not set default locale /data/user/0/com.l`
- `--ownall --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xev: warning: could not set default locale /data/user/0/com.l`
- `--ownall ` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xev: warning: could not set default locale /data/user/0/com.l`
- `--shim --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xev: warning: could not set default locale /data/user/0/com.l`
- `--shim --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xev: warning: could not set default locale /data/user/0/com.l`
- `--shim ` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xev: warning: could not set default locale /data/user/0/com.l`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xfd`

- `--ownall --version` → **rc=1**  
  output: `Error: Can't open display: `
- `--ownall --help` → **rc=1**  
  output: `Error: Can't open display: `
- `--ownall ` → **rc=1**  
  output: `Error: Can't open display: `
- `--shim --version` → **rc=1**  
  output: `Error: Can't open display: `
- `--shim --help` → **rc=1**  
  output: `Error: Can't open display: `
- `--shim ` → **rc=1**  
  output: `Error: Can't open display: `

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xfwm4-workspace-settings`

- `--ownall --version` → **CRASH (SIG11)**  
  output: ` (process:26298): GLib-CRITICAL **: 19:37:22.602: Failed to get RW lock 0x799a580a30: Resource deadlock avoided  (proces`
- `--ownall --help` → **CRASH (SIG11)**  
  output: ` (process:26303): GLib-CRITICAL **: 19:37:24.748: Failed to get RW lock 0x71e384da30: Resource deadlock avoided  (proces`
- `--ownall ` → **CRASH (SIG11)**  
  output: ` (process:26308): GLib-CRITICAL **: 19:37:26.941: Failed to get RW lock 0x73156caa30: Resource deadlock avoided  (proces`
- `--shim --version` → **CRASH (SIG11)**  
  output: ` (process:26321): GLib-CRITICAL **: 19:37:29.116: Failed to get RW lock 0x71d9ab5a30: Resource deadlock avoided  (proces`
- `--shim --help` → **CRASH (SIG11)**  
  output: ` (process:26339): GLib-CRITICAL **: 19:37:31.251: Failed to get RW lock 0x7a96ec1a30: Resource deadlock avoided  (proces`
- `--shim ` → **CRASH (SIG11)**  
  output: ` (process:26354): GLib-CRITICAL **: 19:37:33.366: Failed to get RW lock 0x7058ecfa30: Resource deadlock avoided  (proces`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xkill`

- `--ownall --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xkill: unrecognized argument --version  usage:  /data/user/0/`
- `--ownall --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xkill: unrecognized argument --help  usage:  /data/user/0/com`
- `--ownall ` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xkill:  unable to open display "`
- `--shim --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xkill: unrecognized argument --version  usage:  /data/user/0/`
- `--shim --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xkill: unrecognized argument --help  usage:  /data/user/0/com`
- `--shim ` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xkill:  unable to open display "`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsatoms`

- `--ownall --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsatoms: unrecognized argument --version  usage:  /data/user`
- `--ownall --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsatoms: unrecognized argument --help  usage:  /data/user/0/`
- `--ownall ` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsatoms:  unable to open display "`
- `--shim --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsatoms: unrecognized argument --version  usage:  /data/user`
- `--shim --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsatoms: unrecognized argument --help  usage:  /data/user/0/`
- `--shim ` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsatoms:  unable to open display "`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsclients`

- `--ownall --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsclients: unrecognized argument --version  usage:  /data/us`
- `--ownall --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsclients: unrecognized argument --help  usage:  /data/user/`
- `--ownall ` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsclients:  unable to open display " `
- `--shim --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsclients: unrecognized argument --version  usage:  /data/us`
- `--shim --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsclients: unrecognized argument --help  usage:  /data/user/`
- `--shim ` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsclients:  unable to open display " `

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsfonts`

- `--ownall --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsfonts:  unable to open display ''`
- `--ownall --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsfonts:  unable to open display ''`
- `--ownall ` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsfonts:  unable to open display ''`
- `--shim --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsfonts:  unable to open display ''`
- `--shim --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsfonts:  unable to open display ''`
- `--shim ` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsfonts:  unable to open display ''`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xprop`

- `--ownall --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xprop:  unable to open display ''`
- `--ownall --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xprop:  unable to open display ''`
- `--ownall ` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xprop:  unable to open display ''`
- `--shim --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xprop:  unable to open display ''`
- `--shim --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xprop:  unable to open display ''`
- `--shim ` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xprop:  unable to open display ''`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xwininfo`

- `--ownall --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xwininfo: can not set locale properly /data/user/0/com.linux_`
- `--ownall --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xwininfo: can not set locale properly /data/user/0/com.linux_`
- `--ownall ` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xwininfo: can not set locale properly /data/user/0/com.linux_`
- `--shim --version` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xwininfo: can not set locale properly /data/user/0/com.linux_`
- `--shim --help` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xwininfo: can not set locale properly /data/user/0/com.linux_`
- `--shim ` → **rc=1**  
  output: `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xwininfo: can not set locale properly /data/user/0/com.linux_`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xz`

- `--ownall --version` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=444`
- `--ownall --help` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=444`
- `--ownall ` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=444`
- `--shim --version` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=444`
- `--shim --help` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=444`
- `--shim ` → **CRASH (SIG31)**  
  output: `[SIGSYS] denied syscall nr=444`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/jvm/java-17-openjdk-arm64/bin/jdb`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/jvm/java-17-openjdk-arm64/bin/jdeps`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/jvm/java-17-openjdk-arm64/bin/jhsdb`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/jvm/java-17-openjdk-arm64/bin/jstat`

- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--ownall ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --version` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim --help` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `--shim ` → **rc=2**  
  output: `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`

### `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/jvm/java-17-openjdk-arm64/lib/jspawnhelper`

- `--ownall --version` → **rc=1**  
  output: `Incorrect number of arguments: 2 jspawnhelper version 21.0.7+6-LTS This command is not for general use and should only b`
- `--ownall --help` → **rc=1**  
  output: `Incorrect number of arguments: 2 jspawnhelper version 21.0.7+6-LTS This command is not for general use and should only b`
- `--ownall ` → **rc=1**  
  output: `Incorrect number of arguments: 1 jspawnhelper version 21.0.7+6-LTS This command is not for general use and should only b`
- `--shim --version` → **rc=1**  
  output: `Incorrect number of arguments: 2 jspawnhelper version 21.0.7+6-LTS This command is not for general use and should only b`
- `--shim --help` → **rc=1**  
  output: `Incorrect number of arguments: 2 jspawnhelper version 21.0.7+6-LTS This command is not for general use and should only b`
- `--shim ` → **rc=1**  
  output: `Incorrect number of arguments: 1 jspawnhelper version 21.0.7+6-LTS This command is not for general use and should only b`
- `--ownall --version` → **rc=1**  
  output: `Incorrect number of arguments: 2 jspawnhelper version 25.0.4+7-LTS This command is not for general use and should only b`
- `--ownall --help` → **rc=1**  
  output: `Incorrect number of arguments: 2 jspawnhelper version 25.0.4+7-LTS This command is not for general use and should only b`
- `--ownall ` → **rc=1**  
  output: `Incorrect number of arguments: 1 jspawnhelper version 25.0.4+7-LTS This command is not for general use and should only b`
- `--shim --version` → **rc=1**  
  output: `Incorrect number of arguments: 2 jspawnhelper version 25.0.4+7-LTS This command is not for general use and should only b`
- `--shim --help` → **rc=1**  
  output: `Incorrect number of arguments: 2 jspawnhelper version 25.0.4+7-LTS This command is not for general use and should only b`
- `--shim ` → **rc=1**  
  output: `Incorrect number of arguments: 1 jspawnhelper version 25.0.4+7-LTS This command is not for general use and should only b`

## Clean failure summary — 2026-09-10 21:26:41

Filtered to true ELF executables (ET_EXEC or ET_DYN with PT_INTERP), deduplicated.

Total executables with at least one failure: 136

### Crashes / Timeouts

- `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/glslc`
  - `--ownall --version` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--ownall --help` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--ownall ` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim --version` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim --help` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim ` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/spirv-as`
  - `--ownall --version` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--ownall --help` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--ownall ` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim --version` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim --help` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim ` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/spirv-cfg`
  - `--ownall --version` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--ownall --help` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--ownall ` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim --version` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim --help` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim ` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/spirv-dis`
  - `--ownall --version` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--ownall --help` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--ownall ` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim --version` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim --help` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim ` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/spirv-link`
  - `--ownall --version` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--ownall --help` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--ownall ` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim --version` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim --help` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim ` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/spirv-opt`
  - `--ownall --version` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--ownall --help` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--ownall ` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim --version` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim --help` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim ` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/spirv-reduce`
  - `--ownall --version` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--ownall --help` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--ownall ` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim --version` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim --help` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim ` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64/spirv-val`
  - `--ownall --version` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--ownall --help` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--ownall ` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim --version` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim --help` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
  - `--shim ` → **CRASH (SIG11)** `[-] /data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/shader-tools/linux-x86_64`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/19/lib/linux/x86_64/lldb-server`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 00200000-006e1000 r--p 00000000 00:00 0  006e1000-006e4000 rw-p 00000000 00:00 0  006`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 00200000-006e1000 r--p 00000000 00:00 0  006e1000-006e4000 rw-p 00000000 00:00 0  006`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 00200000-006e1000 r--p 00000000 00:00 0  006e1000-006e4000 rw-p 00000000 00:00 0  006`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 00200000-006e1000 r--p 00000000 00:00 0  006e1000-006e4000 rw-p 00000000 00:00 0  006`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 00200000-006e1000 r--p 00000000 00:00 0  006e1000-006e4000 rw-p 00000000 00:00 0  006`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 00200000-006e1000 r--p 00000000 00:00 0  006e1000-006e4000 rw-p 00000000 00:00 0  006`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/opt/android-ndk-r28/toolchains/llvm/prebuilt/linux-x86_64/python3/bin/python3.11`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-00401000 r-xp 00000000 00:00 0  00401000-00602000 rw-p 00000000 00:00 0  56d`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-00401000 r-xp 00000000 00:00 0  00401000-00602000 rw-p 00000000 00:00 0  5f0`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 00400000-00401000 r-xp 00000000 00:00 0  00401000-00602000 rw-p 00000000 00:00 0  5c0`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-00401000 r-xp 00000000 00:00 0  00401000-00602000 rw-p 00000000 00:00 0  59a`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-00401000 r-xp 00000000 00:00 0  00401000-00602000 rw-p 00000000 00:00 0  558`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 00400000-00401000 r-xp 00000000 00:00 0  00401000-00602000 rw-p 00000000 00:00 0  63e`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.nvm/versions/node/v26.8.1/bin/node`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-0080a000 r--p 00000000 00:00 0  0080a000-00810000 rw-p 00000000 00:00 0  008`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-0080a000 r--p 00000000 00:00 0  0080a000-00810000 rw-p 00000000 00:00 0  008`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 00400000-0080a000 r--p 00000000 00:00 0  0080a000-00810000 rw-p 00000000 00:00 0  008`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-0080a000 r--p 00000000 00:00 0  0080a000-00810000 rw-p 00000000 00:00 0  008`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-0080a000 r--p 00000000 00:00 0  0080a000-00810000 rw-p 00000000 00:00 0  008`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 00400000-0080a000 r--p 00000000 00:00 0  0080a000-00810000 rw-p 00000000 00:00 0  008`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.nvm/versions/node/v26.8.1/lib/node_modules/@earendil-works/pi-coding-agent/node_modules/@esbuild/linux-loong64/bin/esbuild`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 00010000-004b6000 r-xp 00000000 00:00 0  004b6000-004c0000 rw-p 00000000 00:00 0  004`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 00010000-004b6000 r-xp 00000000 00:00 0  004b6000-004c0000 rw-p 00000000 00:00 0  004`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 00010000-004b6000 r-xp 00000000 00:00 0  004b6000-004c0000 rw-p 00000000 00:00 0  004`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 00010000-004b6000 r-xp 00000000 00:00 0  004b6000-004c0000 rw-p 00000000 00:00 0  004`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 00010000-004b6000 r-xp 00000000 00:00 0  004b6000-004c0000 rw-p 00000000 00:00 0  004`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.nvm/versions/node/v26.8.1/lib/node_modules/@github/copilot/node_modules/@github/copilot-linux-arm64/copilot`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-064dd000 r-xp 00000000 00:00 0  064dd000-0e160000 rw-p 00000000 00:00 0  0e1`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-064dd000 r-xp 00000000 00:00 0  064dd000-0e160000 rw-p 00000000 00:00 0  0e1`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 00400000-064dd000 r-xp 00000000 00:00 0  064dd000-0e160000 rw-p 00000000 00:00 0  0e1`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-064dd000 r-xp 00000000 00:00 0  064dd000-0e160000 rw-p 00000000 00:00 0  0e1`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-064dd000 r-xp 00000000 00:00 0  064dd000-0e160000 rw-p 00000000 00:00 0  0e1`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 00400000-064dd000 r-xp 00000000 00:00 0  064dd000-0e160000 rw-p 00000000 00:00 0  0e1`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.nvm/versions/node/v26.8.1/lib/node_modules/@github/copilot/node_modules/@github/copilot-linux-arm64/tgrep/bin/linux-arm64/tgrep`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 6123490000-6123497000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 5df72e9000-5df72f0000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 640f5a7000-640f5ae000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 63b12d2000-63b12d9000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 590c738000-590c73f000 r--p 00000000 fd:29 1236062                        /data/user/0`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.nvm/versions/node/v26.8.1/lib/node_modules/@kilocode/cli/node_modules/@kilocode/cli-linux-arm64/bin/.l2s.kilo0001.0002`
  - `--ownall --version` → **CRASH (SIG11)** `============================================================ Bun v1.3.14 (0d9b296a) Linux arm64 Linu`
  - `--ownall --help` → **CRASH (SIG11)** `============================================================ Bun v1.3.14 (0d9b296a) Linux arm64 Linu`
  - `--ownall ` → **CRASH (SIG11)** `============================================================ Bun v1.3.14 (0d9b296a) Linux arm64 Linu`
  - `--shim --version` → **CRASH (SIG11)** `============================================================ Bun v1.3.14 (0d9b296a) Linux arm64 Linu`
  - `--shim --help` → **CRASH (SIG11)** `============================================================ Bun v1.3.14 (0d9b296a) Linux arm64 Linu`
  - `--shim ` → **CRASH (SIG11)** `============================================================ Bun v1.3.14 (0d9b296a) Linux arm64 Linu`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.nvm/versions/node/v26.8.1/lib/node_modules/@kilocode/cli/node_modules/@kilocode/cli-linux-arm64/bin/kilo-sandbox-seccomp`
  - `--ownall --version` → **CRASH (SIG31)** `[SIGSYS] denied syscall nr=99`
  - `--ownall --help` → **CRASH (SIG31)** `[SIGSYS] denied syscall nr=99`
  - `--ownall ` → **CRASH (SIG31)** `[SIGSYS] denied syscall nr=99`
  - `--shim --version` → **CRASH (SIG31)** `[SIGSYS] denied syscall nr=99`
  - `--shim --help` → **CRASH (SIG31)** `[SIGSYS] denied syscall nr=99`
  - `--shim ` → **CRASH (SIG31)** `[SIGSYS] denied syscall nr=99`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/libexec/version`
  - `--ownall --version` → **CRASH (SIG11)** ` thread 'main' (20726) panicked at src/lib.rs:38:13: not a valid path: /data/user/0/com.linux_core/f`
  - `--ownall --help` → **CRASH (SIG11)** ` thread 'main' (20731) panicked at src/lib.rs:38:13: not a valid path: /data/user/0/com.linux_core/f`
  - `--ownall ` → **CRASH (SIG11)** ` thread 'main' (20736) panicked at src/lib.rs:38:13: not a valid path: /data/user/0/com.linux_core/f`
  - `--shim --version` → **CRASH (SIG11)** ` thread 'main' (20741) panicked at src/lib.rs:38:13: not a valid path: /data/user/0/com.linux_core/f`
  - `--shim --help` → **CRASH (SIG11)** ` thread 'main' (20746) panicked at src/lib.rs:38:13: not a valid path: /data/user/0/com.linux_core/f`
  - `--shim ` → **CRASH (SIG11)** ` thread 'main' (20751) panicked at src/lib.rs:38:13: not a valid path: /data/user/0/com.linux_core/f`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/docs_config_memo/.gemini/antigravity-cli/bin/webm_encoder`
  - `--ownall --version` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--ownall --help` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--ownall ` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--shim --version` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--shim --help` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--shim ` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/elf_loader/elf_loader`
  - `--ownall --version` → **CRASH (SIG11)** `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-li`
  - `--ownall --help` → **CRASH (SIG11)** `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-li`
  - `--ownall ` → **CRASH (SIG11)** `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-li`
  - `--shim --version` → **CRASH (SIG11)** `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-li`
  - `--shim --help` → **CRASH (SIG11)** `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-li`
  - `--shim ` → **CRASH (SIG11)** `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-li`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/gbsh/magisk-module/system/bin/gbsh`
  - `--ownall --version` → **CRASH (SIG11)** `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-li`
  - `--ownall --help` → **CRASH (SIG11)** `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-li`
  - `--ownall ` → **CRASH (SIG11)** `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-li`
  - `--shim --version` → **CRASH (SIG11)** `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-li`
  - `--shim --help` → **CRASH (SIG11)** `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-li`
  - `--shim ` → **CRASH (SIG11)** `[-] dep libdl.so not found [-] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-li`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/kali_combined/kali_core_emulator/app/src/main/assets/usr/bin/rg`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 58aeece000-58aeed5000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 62ab673000-62ab67a000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 56e8669000-56e8670000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 59de7f6000-59de7fd000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 6048c49000-6048c50000 r--p 00000000 fd:29 1236062                        /data/user/0`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/kali_combined/kali_core_emulator/app/src/main/jniLibs/x86_64/libloader.so`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 00200000-00201000 r--p 00000000 00:00 0  00201000-2000000000 rw-p 00000000 00:00 0  2`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 00200000-00201000 r--p 00000000 00:00 0  00201000-2000000000 rw-p 00000000 00:00 0  2`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 00200000-00201000 r--p 00000000 00:00 0  00201000-2000000000 rw-p 00000000 00:00 0  2`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 00200000-00201000 r--p 00000000 00:00 0  00201000-2000000000 rw-p 00000000 00:00 0  2`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 00200000-00201000 r--p 00000000 00:00 0  00201000-2000000000 rw-p 00000000 00:00 0  2`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 00200000-00201000 r--p 00000000 00:00 0  00201000-2000000000 rw-p 00000000 00:00 0  2`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/kali_combined/kali_core_emulator/app/src/main/jniLibs/x86_64/libproot.so`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 61614fd000-6161504000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 5bb3b2b000-5bb3b32000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 59597aa000-59597b1000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 64a9947000-64a994e000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 642d651000-642d658000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 5fd2424000-5fd242b000 r--p 00000000 fd:29 1236062                        /data/user/0`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/aarch64-linux-gnu-gcc-14`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-00506000 r-xp 00000000 00:00 0  00506000-00525000 rw-p 00000000 00:00 0  619`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-00506000 r-xp 00000000 00:00 0  00506000-00525000 rw-p 00000000 00:00 0  588`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 00400000-00506000 r-xp 00000000 00:00 0  00506000-00525000 rw-p 00000000 00:00 0  5d6`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-00506000 r-xp 00000000 00:00 0  00506000-00525000 rw-p 00000000 00:00 0  62f`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-00506000 r-xp 00000000 00:00 0  00506000-00525000 rw-p 00000000 00:00 0  602`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 00400000-00506000 r-xp 00000000 00:00 0  00506000-00525000 rw-p 00000000 00:00 0  613`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/aarch64-linux-gnu-gcc-nm-14`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  651`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  622`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  62f`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  635`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  563`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  59e`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/aarch64-linux-gnu-gcc-ranlib-14`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  597`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  5c9`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  595`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  5a7`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  627`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 00400000-00405000 r-xp 00000000 00:00 0  00405000-00421000 rw-p 00000000 00:00 0  62b`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/aarch64-linux-gnu-gcov-14`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-00471000 r-xp 00000000 00:00 0  00471000-00491000 rw-p 00000000 00:00 0  558`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-00471000 r-xp 00000000 00:00 0  00471000-00491000 rw-p 00000000 00:00 0  627`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 00400000-00471000 r-xp 00000000 00:00 0  00471000-00491000 rw-p 00000000 00:00 0  5b6`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-00471000 r-xp 00000000 00:00 0  00471000-00491000 rw-p 00000000 00:00 0  566`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-00471000 r-xp 00000000 00:00 0  00471000-00491000 rw-p 00000000 00:00 0  63a`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 00400000-00471000 r-xp 00000000 00:00 0  00471000-00491000 rw-p 00000000 00:00 0  61d`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/aarch64-linux-gnu-gcov-dump-14`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-00459000 r-xp 00000000 00:00 0  00459000-00471000 rw-p 00000000 00:00 0  614`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-00459000 r-xp 00000000 00:00 0  00459000-00471000 rw-p 00000000 00:00 0  5bb`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 00400000-00459000 r-xp 00000000 00:00 0  00459000-00471000 rw-p 00000000 00:00 0  5f8`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-00459000 r-xp 00000000 00:00 0  00459000-00471000 rw-p 00000000 00:00 0  5a5`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-00459000 r-xp 00000000 00:00 0  00459000-00471000 rw-p 00000000 00:00 0  5a2`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 00400000-00459000 r-xp 00000000 00:00 0  00459000-00471000 rw-p 00000000 00:00 0  59e`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/aarch64-linux-gnu-gcov-tool-14`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-00461000 r-xp 00000000 00:00 0  00461000-00481000 rw-p 00000000 00:00 0  565`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-00461000 r-xp 00000000 00:00 0  00461000-00481000 rw-p 00000000 00:00 0  63a`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 00400000-00461000 r-xp 00000000 00:00 0  00461000-00481000 rw-p 00000000 00:00 0  55e`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-00461000 r-xp 00000000 00:00 0  00461000-00481000 rw-p 00000000 00:00 0  55a`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-00461000 r-xp 00000000 00:00 0  00461000-00481000 rw-p 00000000 00:00 0  555`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 00400000-00461000 r-xp 00000000 00:00 0  00461000-00481000 rw-p 00000000 00:00 0  620`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/aarch64-linux-gnu-lto-dump-14`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-01fd0000 r-xp 00000000 00:00 0  01fd0000-02201000 rw-p 00000000 00:00 0  558`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-01fd0000 r-xp 00000000 00:00 0  01fd0000-02201000 rw-p 00000000 00:00 0  5f1`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 00400000-01fd0000 r-xp 00000000 00:00 0  01fd0000-02201000 rw-p 00000000 00:00 0  5a3`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 00400000-01fd0000 r-xp 00000000 00:00 0  01fd0000-02201000 rw-p 00000000 00:00 0  5d0`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 00400000-01fd0000 r-xp 00000000 00:00 0  01fd0000-02201000 rw-p 00000000 00:00 0  5d0`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 00400000-01fd0000 r-xp 00000000 00:00 0  01fd0000-02201000 rw-p 00000000 00:00 0  559`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/fzf`
  - `--ownall --version` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--ownall --help` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--ownall ` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--shim --version` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--shim --help` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--shim ` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/gh`
  - `--ownall --version` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--ownall --help` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--ownall ` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--shim --version` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--shim --help` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--shim ` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/git-lfs`
  - `--ownall --version` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--ownall --help` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--ownall ` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--shim --version` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--shim --help` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
  - `--shim ` → **CRASH (SIG6)** `Fatal glibc error: allocatestack.c:335 (allocate_stack): assertion failed: size != 0`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/groff`
  - `--ownall --version` → **CRASH (SIG11)** `GNU groff version 1.23.0 Copyright (C) 2022 Free Software Foundation, Inc. GNU groff comes with ABSO`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/lsof`
  - `--ownall ` → **TIMEOUT** ``
  - `--shim ` → **TIMEOUT** ``
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/luit`
  - `--ownall ` → **CRASH (SIG11)** `Warning: couldn't set locale. Warning: couldn't find charset data for locale C.UTF-8; using ISO 8859`
  - `--shim ` → **CRASH (SIG11)** `Warning: couldn't set locale. Warning: couldn't find charset data for locale C.UTF-8; using ISO 8859`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/man-recode`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 5cd179d000-5cd17a4000 r--p 00000000 fd:29 1236062                        /data/user/0`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ps`
  - `--ownall --version` → **CRASH (SIG11)** `Signal 11 (SEGV) caught by ps (4.0.4). /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ps`
  - `--ownall --help` → **CRASH (SIG11)** `Signal 11 (SEGV) caught by ps (4.0.4). /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ps`
  - `--ownall ` → **CRASH (SIG11)** `Signal 11 (SEGV) caught by ps (4.0.4). /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ps`
  - `--shim --version` → **CRASH (SIG11)** `Signal 11 (SEGV) caught by ps (4.0.4). /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ps`
  - `--shim --help` → **CRASH (SIG11)** `Signal 11 (SEGV) caught by ps (4.0.4). /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ps`
  - `--shim ` → **CRASH (SIG11)** `Signal 11 (SEGV) caught by ps (4.0.4). /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ps`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/qemu-aarch64-static`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 60000000-60001000 r--p 00000000 00:00 0  60001000-6031d000 r-xp 00000000 00:00 0  603`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 60000000-60001000 r--p 00000000 00:00 0  60001000-6031d000 r-xp 00000000 00:00 0  603`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 60000000-60001000 r--p 00000000 00:00 0  60001000-6031d000 r-xp 00000000 00:00 0  603`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 60000000-60001000 r--p 00000000 00:00 0  60001000-6031d000 r-xp 00000000 00:00 0  603`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 60000000-60001000 r--p 00000000 00:00 0  60001000-6031d000 r-xp 00000000 00:00 0  603`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 60000000-60001000 r--p 00000000 00:00 0  60001000-6031d000 r-xp 00000000 00:00 0  603`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/scp`
  - `--ownall ` → **CRASH (SIG127)** `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/scp: unknown user 10315 `
  - `--shim ` → **CRASH (SIG127)** `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/scp: unknown user 10315 `
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ssh-add`
  - `--ownall --version` → **CRASH (SIG127)** `PRNG is not seeded `
  - `--ownall --help` → **CRASH (SIG127)** `PRNG is not seeded `
  - `--ownall ` → **CRASH (SIG127)** `PRNG is not seeded `
  - `--shim --version` → **CRASH (SIG127)** `PRNG is not seeded `
  - `--shim --help` → **CRASH (SIG127)** `PRNG is not seeded `
  - `--shim ` → **CRASH (SIG127)** `PRNG is not seeded `
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ssh-agent`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 582bde3000-582bdea000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 65188b8000-65188bf000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 5b7f80c000-5b7f813000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 5ee75c5000-5ee75cc000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 627877a000-6278781000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 5fa15fd000-5fa1604000 r--p 00000000 fd:29 1236062                        /data/user/0`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ssh-keygen`
  - `--ownall --version` → **CRASH (SIG127)** `PRNG is not seeded `
  - `--ownall --help` → **CRASH (SIG127)** `PRNG is not seeded `
  - `--ownall ` → **CRASH (SIG127)** `PRNG is not seeded `
  - `--shim --version` → **CRASH (SIG127)** `PRNG is not seeded `
  - `--shim --help` → **CRASH (SIG127)** `PRNG is not seeded `
  - `--shim ` → **CRASH (SIG127)** `PRNG is not seeded `
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ssh-keyscan`
  - `--ownall --version` → **CRASH (SIG127)** `PRNG is not seeded `
  - `--ownall --help` → **CRASH (SIG127)** `PRNG is not seeded `
  - `--ownall ` → **CRASH (SIG127)** `PRNG is not seeded `
  - `--shim --version` → **CRASH (SIG127)** `PRNG is not seeded `
  - `--shim --help` → **CRASH (SIG127)** `PRNG is not seeded `
  - `--shim ` → **CRASH (SIG127)** `PRNG is not seeded `
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/starship`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 5f30bb8000-5f30bbf000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 5ce7441000-5ce7448000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 5ab3df1000-5ab3df8000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 588f596000-588f59d000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 63dfc95000-63dfc9c000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 62df571000-62df578000 r--p 00000000 fd:29 1236062                        /data/user/0`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/udisksctl`
  - `--ownall --version` → **CRASH (SIG11)** ` (process:20798): GLib-CRITICAL **: 19:35:39.145: Failed to get RW lock 0x736be9ba30: Resource deadl`
  - `--ownall --help` → **CRASH (SIG11)** ` (process:20803): GLib-CRITICAL **: 19:35:39.516: Failed to get RW lock 0x769c961a30: Resource deadl`
  - `--shim --version` → **CRASH (SIG11)** ` (process:20817): GLib-CRITICAL **: 19:35:40.269: Failed to get RW lock 0x719fc10a30: Resource deadl`
  - `--shim --help` → **CRASH (SIG11)** ` (process:20822): GLib-CRITICAL **: 19:35:40.632: Failed to get RW lock 0x6f565e5a30: Resource deadl`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/wmdocker`
  - `--ownall ` → **CRASH (SIG11)** `Segmentation Fault or Critical Error encountered. Dumping core and aborting.`
  - `--shim ` → **CRASH (SIG11)** `Segmentation Fault or Critical Error encountered. Dumping core and aborting.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdriinfo`
  - `--ownall --version` → **CRASH (SIG11)** `  [maps-begin] 57ca187000-57ca18e000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--ownall --help` → **CRASH (SIG11)** `  [maps-begin] 5e10613000-5e1061a000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--ownall ` → **CRASH (SIG11)** `  [maps-begin] 577b922000-577b929000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim --version` → **CRASH (SIG11)** `  [maps-begin] 64bc112000-64bc119000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim --help` → **CRASH (SIG11)** `  [maps-begin] 5f54a88000-5f54a8f000 r--p 00000000 fd:29 1236062                        /data/user/0`
  - `--shim ` → **CRASH (SIG11)** `  [maps-begin] 5cd467b000-5cd4682000 r--p 00000000 fd:29 1236062                        /data/user/0`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xfwm4-workspace-settings`
  - `--ownall --version` → **CRASH (SIG11)** ` (process:26298): GLib-CRITICAL **: 19:37:22.602: Failed to get RW lock 0x799a580a30: Resource deadl`
  - `--ownall --help` → **CRASH (SIG11)** ` (process:26303): GLib-CRITICAL **: 19:37:24.748: Failed to get RW lock 0x71e384da30: Resource deadl`
  - `--ownall ` → **CRASH (SIG11)** ` (process:26308): GLib-CRITICAL **: 19:37:26.941: Failed to get RW lock 0x73156caa30: Resource deadl`
  - `--shim --version` → **CRASH (SIG11)** ` (process:26321): GLib-CRITICAL **: 19:37:29.116: Failed to get RW lock 0x71d9ab5a30: Resource deadl`
  - `--shim --help` → **CRASH (SIG11)** ` (process:26339): GLib-CRITICAL **: 19:37:31.251: Failed to get RW lock 0x7a96ec1a30: Resource deadl`
  - `--shim ` → **CRASH (SIG11)** ` (process:26354): GLib-CRITICAL **: 19:37:33.366: Failed to get RW lock 0x7058ecfa30: Resource deadl`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xz`
  - `--ownall --version` → **CRASH (SIG31)** `[SIGSYS] denied syscall nr=444`
  - `--ownall --help` → **CRASH (SIG31)** `[SIGSYS] denied syscall nr=444`
  - `--ownall ` → **CRASH (SIG31)** `[SIGSYS] denied syscall nr=444`
  - `--shim --version` → **CRASH (SIG31)** `[SIGSYS] denied syscall nr=444`
  - `--shim --help` → **CRASH (SIG31)** `[SIGSYS] denied syscall nr=444`
  - `--shim ` → **CRASH (SIG31)** `[SIGSYS] denied syscall nr=444`

### Non-zero exits

- `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/jar`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/javap`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/jcmd`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/jdb`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/jmod`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/jpackage`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/jshell`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/jstat`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/keytool`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/etc/alternatives/serialver`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.cache/copilot/pkg/linux-arm64/1.0.82/prebuilds/linux-arm64/copilot-runtime`
  - `--ownall --version` → rc=1 `copilot-runtime: unsupported argument '--version'`
  - `--ownall --help` → rc=1 `copilot-runtime: unsupported argument '--help'`
  - `--ownall ` → rc=1 `copilot-runtime: SDK server mode requires --server or --headless`
  - `--shim --version` → rc=1 `copilot-runtime: unsupported argument '--version'`
  - `--shim --help` → rc=1 `copilot-runtime: unsupported argument '--help'`
  - `--shim ` → rc=1 `copilot-runtime: SDK server mode requires --server or --headless`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.cache/uv/archive-v0/t2xQydCYlmEA5xh5/uv-0.12.9.data/scripts/uvx`
  - `--ownall --version` → rc=2 `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
  - `--ownall --help` → rc=2 `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
  - `--ownall ` → rc=2 `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
  - `--shim --version` → rc=2 `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
  - `--shim --help` → rc=2 `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
  - `--shim ` → rc=2 `error: Could not find the `uv` binary at: /data/user/0/com.linux_core/files/usr/bin/uv`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.gradle/jdks/eclipse_adoptium-21-aarch64-linux.2/bin/javac`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.gradle/jdks/eclipse_adoptium-21-aarch64-linux.2/bin/javadoc`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.gradle/jdks/eclipse_adoptium-21-aarch64-linux.2/bin/jconsole`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.gradle/jdks/eclipse_adoptium-21-aarch64-linux.2/bin/jps`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.gradle/jdks/eclipse_adoptium-21-aarch64-linux.2/bin/jstatd`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.gradle/jdks/eclipse_adoptium-21-aarch64-linux.2/bin/jwebserver`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.gradle/jdks/eclipse_adoptium-21-aarch64-linux.2/bin/rmiregistry`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/candidates/java/25.0.4-tem/bin/java`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/candidates/java/25.0.4-tem/bin/jdeprscan`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/candidates/java/25.0.4-tem/bin/jdeps`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/candidates/java/25.0.4-tem/bin/jinfo`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/candidates/java/25.0.4-tem/bin/jlink`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/candidates/java/25.0.4-tem/bin/jmap`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/candidates/java/25.0.4-tem/bin/jnativescan`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/candidates/java/25.0.4-tem/bin/jrunscript`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/candidates/java/25.0.4-tem/lib/jspawnhelper`
  - `--ownall --version` → rc=1 `Incorrect number of arguments: 2 jspawnhelper version 21.0.7+6-LTS This command is not for general u`
  - `--ownall --help` → rc=1 `Incorrect number of arguments: 2 jspawnhelper version 21.0.7+6-LTS This command is not for general u`
  - `--ownall ` → rc=1 `Incorrect number of arguments: 1 jspawnhelper version 21.0.7+6-LTS This command is not for general u`
  - `--shim --version` → rc=1 `Incorrect number of arguments: 2 jspawnhelper version 21.0.7+6-LTS This command is not for general u`
  - `--shim --help` → rc=1 `Incorrect number of arguments: 2 jspawnhelper version 21.0.7+6-LTS This command is not for general u`
  - `--shim ` → rc=1 `Incorrect number of arguments: 1 jspawnhelper version 21.0.7+6-LTS This command is not for general u`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/libexec/current`
  - `--ownall --version` → rc=2 `error: unexpected argument '--version' found    tip: to pass '--version' as a value, use '-- --versi`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/libexec/default`
  - `--ownall --version` → rc=2 `error: unexpected argument '--version' found    tip: to pass '--version' as a value, use '-- --versi`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/libexec/help`
  - `--ownall --version` → rc=2 `error: unexpected argument '--version' found  Usage: help [COMMAND]  For more information, try '--he`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/libexec/home`
  - `--ownall --version` → rc=2 `error: unexpected argument '--version' found    tip: to pass '--version' as a value, use '-- --versi`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/.sdkman/libexec/uninstall`
  - `--ownall --version` → rc=2 `error: unexpected argument '--version' found    tip: to pass '--version' as a value, use '-- --versi`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/docs_config_memo/.local/share/gh/extensions/gh-models/gh-models`
  - `--ownall --version` → rc=1 `No GitHub token found. Please run 'gh auth login' to authenticate. Error: unknown flag: --version Us`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/root/docs_config_memo/.local/share/gh/extensions/gh-s/gh-s`
  - `--ownall --version` → rc=1 `gh-s 0.0.12`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/busybox`
  - `--ownall --version` → rc=127 `--version: applet not found`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/catman`
  - `--ownall --version` → rc=64 `catman: can't set the locale; make sure $LC_* and $LANG are correct /data/user/0/com.linux_core/file`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chacl`
  - `--ownall --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chacl: invalid option -- '-' Usage: 	chac`
  - `--ownall --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chacl: invalid option -- '-' Usage: 	chac`
  - `--ownall ` → rc=1 `Usage: 	chacl acl pathname... 	chacl -b acl dacl pathname... 	chacl -d dacl pathname... 	chacl -R pa`
  - `--shim --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chacl: invalid option -- '-' Usage: 	chac`
  - `--shim --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chacl: invalid option -- '-' Usage: 	chac`
  - `--shim ` → rc=1 `Usage: 	chacl acl pathname... 	chacl -b acl dacl pathname... 	chacl -d dacl pathname... 	chacl -R pa`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chage`
  - `--ownall --version` → rc=1 `Cannot open audit interface - aborting.`
  - `--ownall --help` → rc=1 `Cannot open audit interface - aborting.`
  - `--ownall ` → rc=1 `Cannot open audit interface - aborting.`
  - `--shim --version` → rc=1 `Cannot open audit interface - aborting.`
  - `--shim --help` → rc=1 `Cannot open audit interface - aborting.`
  - `--shim ` → rc=1 `Cannot open audit interface - aborting.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chattr`
  - `--ownall --version` → rc=1 `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chattr [-RVf] [-+=aAcCdDeijPsStTuF`
  - `--ownall --help` → rc=1 `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chattr [-RVf] [-+=aAcCdDeijPsStTuF`
  - `--ownall ` → rc=1 `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chattr [-RVf] [-+=aAcCdDeijPsStTuF`
  - `--shim --version` → rc=1 `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chattr [-RVf] [-+=aAcCdDeijPsStTuF`
  - `--shim --help` → rc=1 `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chattr [-RVf] [-+=aAcCdDeijPsStTuF`
  - `--shim ` → rc=1 `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/chattr [-RVf] [-+=aAcCdDeijPsStTuF`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-monitor`
  - `--ownall --version` → rc=1 `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-monitor [--system | --session`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-send`
  - `--ownall --version` → rc=1 `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-send [--help] [--system | --s`
  - `--ownall --help` → rc=1 `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-send [--help] [--system | --s`
  - `--ownall ` → rc=1 `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-send [--help] [--system | --s`
  - `--shim --version` → rc=1 `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-send [--help] [--system | --s`
  - `--shim --help` → rc=1 `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-send [--help] [--system | --s`
  - `--shim ` → rc=1 `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-send [--help] [--system | --s`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dbus-update-activation-environment`
  - `--ownall --version` → rc=64 `dbus-update-activation-environment: update environment variables that will be set for D-Bus     sess`
  - `--ownall --help` → rc=64 `dbus-update-activation-environment: update environment variables that will be set for D-Bus     sess`
  - `--ownall ` → rc=71 `dbus-update-activation-environment: error: unable to connect to D-Bus: Unable to autolaunch a dbus-d`
  - `--shim --version` → rc=64 `dbus-update-activation-environment: update environment variables that will be set for D-Bus     sess`
  - `--shim --help` → rc=64 `dbus-update-activation-environment: update environment variables that will be set for D-Bus     sess`
  - `--shim ` → rc=71 `dbus-update-activation-environment: error: unable to connect to D-Bus: Unable to autolaunch a dbus-d`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearkey`
  - `--ownall --version` → rc=1 `Unknown argument --version Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbea`
  - `--ownall --help` → rc=1 `Unknown argument --help Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearke`
  - `--ownall ` → rc=1 `Must specify a key filename Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbe`
  - `--shim --version` → rc=1 `Unknown argument --version Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbea`
  - `--shim --help` → rc=1 `Unknown argument --help Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbearke`
  - `--shim ` → rc=1 `Must specify a key filename Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dropbe`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/funzip`
  - `--ownall --version` → rc=3 `funzip error: input not a zip or gzip file`
  - `--ownall --help` → rc=3 `funzip error: input not a zip or gzip file`
  - `--ownall ` → rc=3 `funzip error: input not a zip or gzip file`
  - `--shim --version` → rc=3 `funzip error: input not a zip or gzip file`
  - `--shim --help` → rc=3 `funzip error: input not a zip or gzip file`
  - `--shim ` → rc=3 `funzip error: input not a zip or gzip file`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/gencat`
  - `--ownall --version` → rc=64 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/gencat: unrecognized option '--version' T`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/getent`
  - `--ownall --version` → rc=64 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/getent: unrecognized option '--version' T`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/gpgparsemail`
  - `--ownall --version` → rc=1 `gpgparsemail: can't open '--version': No such file or directory`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/html2text`
  - `--ownall --version` → rc=1 `Unrecognized command line option --version"`
  - `--ownall --help` → rc=1 `Unrecognized command line option --help"`
  - `--ownall ` → rc=1 `Opening input file -": invalid from_encoding"`
  - `--shim --version` → rc=1 `Unrecognized command line option --version"`
  - `--shim --help` → rc=1 `Unrecognized command line option --help"`
  - `--shim ` → rc=1 `Opening input file -": invalid from_encoding"`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/iconv`
  - `--ownall --version` → rc=64 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/iconv: unrecognized option '--version' Tr`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/infocmp`
  - `--ownall --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/infocmp: invalid option -- '-' Usage: inf`
  - `--ownall --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/infocmp: invalid option -- '-' Usage: inf`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/jarsigner`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/jhsdb`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/jstack`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/lexgrog`
  - `--ownall --version` → rc=64 `lexgrog: can't set the locale; make sure $LC_* and $LANG are correct /data/user/0/com.linux_core/fil`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/locale`
  - `--ownall --version` → rc=64 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/locale: Cannot set LC_CTYPE to default lo`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/localedef`
  - `--ownall --version` → rc=4 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/localedef: unrecognized option '--version`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/lsattr`
  - `--ownall --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/lsattr: invalid option -- '-' Usage: /dat`
  - `--ownall --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/lsattr: invalid option -- '-' Usage: /dat`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/lsof`
  - `--ownall --version` → rc=1 `lsof: illegal option character: - lsof: -e not followed by a file system path: rsion" lsof 4.99.4  l`
  - `--ownall --help` → rc=1 `lsof: illegal option character: - lsof: -e not followed by a file system path: lp" lsof 4.99.4  late`
  - `--shim --version` → rc=1 `lsof: illegal option character: - lsof: -e not followed by a file system path: rsion" lsof 4.99.4  l`
  - `--shim --help` → rc=1 `lsof: illegal option character: - lsof: -e not followed by a file system path: lp" lsof 4.99.4  late`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/luit`
  - `--ownall --version` → rc=1 `Warning: couldn't set locale. Unknown option --version /data/user/0/com.linux_core/files/nh/distro/p`
  - `--ownall --help` → rc=1 `Warning: couldn't set locale. Unknown option --help /data/user/0/com.linux_core/files/nh/distro/parr`
  - `--shim --version` → rc=1 `Warning: couldn't set locale. Unknown option --version /data/user/0/com.linux_core/files/nh/distro/p`
  - `--shim --help` → rc=1 `Warning: couldn't set locale. Unknown option --help /data/user/0/com.linux_core/files/nh/distro/parr`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/man`
  - `--ownall --version` → rc=64 `man: can't set the locale; make sure $LC_* and $LANG are correct /data/user/0/com.linux_core/files/n`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/man-recode`
  - `--ownall --version` → rc=64 `man-recode: can't set the locale; make sure $LC_* and $LANG are correct /data/user/0/com.linux_core/`
  - `--ownall ` → rc=64 `man-recode: can't set the locale; make sure $LC_* and $LANG are correct Usage: man-recode [OPTION...`
  - `--shim --version` → rc=64 `man-recode: can't set the locale; make sure $LC_* and $LANG are correct /data/user/0/com.linux_core/`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/mandb`
  - `--ownall --version` → rc=64 `mandb: can't set the locale; make sure $LC_* and $LANG are correct /data/user/0/com.linux_core/files`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/manpath`
  - `--ownall --version` → rc=64 `manpath: can't set the locale; make sure $LC_* and $LANG are correct /data/user/0/com.linux_core/fil`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/peekfd`
  - `--ownall --version` → rc=1 `peekfd (PSmisc) 23.7 Copyright (C) 2007 Trent Waddington  PSmisc comes with ABSOLUTELY NO WARRANTY. `
  - `--ownall --help` → rc=1 `Usage: peekfd [-8] [-n] [-c] [-d] [-V] [-h] <pid> [<fd> ..]     -8, --eight-bit-clean        output `
  - `--ownall ` → rc=1 `Usage: peekfd [-8] [-n] [-c] [-d] [-V] [-h] <pid> [<fd> ..]     -8, --eight-bit-clean        output `
  - `--shim --version` → rc=1 `peekfd (PSmisc) 23.7 Copyright (C) 2007 Trent Waddington  PSmisc comes with ABSOLUTELY NO WARRANTY. `
  - `--shim --help` → rc=1 `Usage: peekfd [-8] [-n] [-c] [-d] [-V] [-h] <pid> [<fd> ..]     -8, --eight-bit-clean        output `
  - `--shim ` → rc=1 `Usage: peekfd [-8] [-n] [-c] [-d] [-V] [-h] <pid> [<fd> ..]     -8, --eight-bit-clean        output `
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/pldd`
  - `--ownall --version` → rc=64 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/pldd: unrecognized option '--version' Try`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/scp`
  - `--ownall --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/scp: unknown option -- -  usage: scp [-34`
  - `--ownall --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/scp: unknown option -- -  usage: scp [-34`
  - `--shim --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/scp: unknown option -- -  usage: scp [-34`
  - `--shim --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/scp: unknown option -- -  usage: scp [-34`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/sftp`
  - `--ownall --version` → rc=1 `unknown option -- -  usage: sftp [-46AaCfNpqrv] [-B buffer_size] [-b batchfile] [-c cipher]         `
  - `--ownall --help` → rc=1 `unknown option -- -  usage: sftp [-46AaCfNpqrv] [-B buffer_size] [-b batchfile] [-c cipher]         `
  - `--ownall ` → rc=1 `usage: sftp [-46AaCfNpqrv] [-B buffer_size] [-b batchfile] [-c cipher]           [-D sftp_server_com`
  - `--shim --version` → rc=1 `unknown option -- -  usage: sftp [-46AaCfNpqrv] [-B buffer_size] [-b batchfile] [-c cipher]         `
  - `--shim --help` → rc=1 `unknown option -- -  usage: sftp [-46AaCfNpqrv] [-B buffer_size] [-b batchfile] [-c cipher]         `
  - `--shim ` → rc=1 `usage: sftp [-46AaCfNpqrv] [-B buffer_size] [-b batchfile] [-c cipher]           [-D sftp_server_com`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/sudo.orig`
  - `--ownall --version` → rc=1 `sudo: /etc/sudo.conf is owned by uid 10315, should be 0 sudo: The no new privileges" flag is set`
  - `--ownall --help` → rc=1 `sudo: /etc/sudo.conf is owned by uid 10315, should be 0 sudo: The no new privileges" flag is set`
  - `--ownall ` → rc=1 `sudo: /etc/sudo.conf is owned by uid 10315, should be 0 sudo: The no new privileges" flag is set`
  - `--shim --version` → rc=1 `sudo: /etc/sudo.conf is owned by uid 10315, should be 0 sudo: The no new privileges" flag is set`
  - `--shim --help` → rc=1 `sudo: /etc/sudo.conf is owned by uid 10315, should be 0 sudo: The no new privileges" flag is set`
  - `--shim ` → rc=1 `sudo: /etc/sudo.conf is owned by uid 10315, should be 0 sudo: The no new privileges" flag is set`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tabs`
  - `--ownall --version` → rc=1 `Usage: tabs [options] [tabstop-list]  Options:   -0       reset tabs   -8       set tabs to standard`
  - `--ownall --help` → rc=1 `Usage: tabs [options] [tabstop-list]  Options:   -0       reset tabs   -8       set tabs to standard`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tic`
  - `--ownall --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tic: invalid option -- '-' Usage: tic [-e`
  - `--ownall --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tic: invalid option -- '-' Usage: tic [-e`
  - `--ownall ` → rc=1 `tic: File name needed.  Usage: 	tic [-e names] [-o dir] [-R name] [-v[n]] [-V] [-w[n]] [-1aCDcfGgIKL`
  - `--shim --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tic: invalid option -- '-' Usage: tic [-e`
  - `--shim --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tic: invalid option -- '-' Usage: tic [-e`
  - `--shim ` → rc=1 `tic: File name needed.  Usage: 	tic [-e names] [-o dir] [-R name] [-v[n]] [-V] [-w[n]] [-1aCDcfGgIKL`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/toe`
  - `--ownall --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/toe: invalid option -- '-' usage: toe [-a`
  - `--ownall --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/toe: invalid option -- '-' usage: toe [-a`
  - `--shim --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/toe: invalid option -- '-' usage: toe [-a`
  - `--shim --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/toe: invalid option -- '-' usage: toe [-a`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tput`
  - `--ownall --version` → rc=2 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tput: invalid option -- '-' Usage: tput [`
  - `--ownall --help` → rc=2 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tput: invalid option -- '-' Usage: tput [`
  - `--ownall ` → rc=2 `Usage: tput [options] [command]  Options:   -S <<       read commands from standard input   -T TERM `
  - `--shim --version` → rc=2 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tput: invalid option -- '-' Usage: tput [`
  - `--shim --help` → rc=2 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tput: invalid option -- '-' Usage: tput [`
  - `--shim ` → rc=2 `Usage: tput [options] [command]  Options:   -S <<       read commands from standard input   -T TERM `
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/udisksctl`
  - `--ownall ` → rc=1 ` (process:20808): GLib-CRITICAL **: 19:35:39.891: Failed to get RW lock 0x7ac792ba30: Resource deadl`
  - `--shim ` → rc=1 ` (process:20827): GLib-CRITICAL **: 19:35:41.004: Failed to get RW lock 0x7ea0582a30: Resource deadl`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/update-mime-database`
  - `--ownall --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/update-mime-database: invalid option -- '`
  - `--ownall --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/update-mime-database: invalid option -- '`
  - `--ownall ` → rc=1 `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/update-mime-database [-hvVn] MIME-`
  - `--shim --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/update-mime-database: invalid option -- '`
  - `--shim --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/update-mime-database: invalid option -- '`
  - `--shim ` → rc=1 `Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/update-mime-database [-hvVn] MIME-`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/viewres`
  - `--ownall --version` → rc=1 `Error: Can't open display: `
  - `--ownall --help` → rc=1 `Error: Can't open display: `
  - `--ownall ` → rc=1 `Error: Can't open display: `
  - `--shim --version` → rc=1 `Error: Can't open display: `
  - `--shim --help` → rc=1 `Error: Can't open display: `
  - `--shim ` → rc=1 `Error: Can't open display: `
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/whatis`
  - `--ownall --version` → rc=64 `whatis: can't set the locale; make sure $LC_* and $LANG are correct /data/user/0/com.linux_core/file`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/wmdocker`
  - `--ownall --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/wmdocker - version 1.5 Copyright 2003, Be`
  - `--ownall --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/wmdocker - version 1.5 Copyright 2003, Be`
  - `--shim --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/wmdocker - version 1.5 Copyright 2003, Be`
  - `--shim --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/wmdocker - version 1.5 Copyright 2003, Be`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdg-user-dirs-update`
  - `--ownall --version` → rc=1 `Invalid argument --version`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdpyinfo`
  - `--ownall --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdpyinfo: unrecognized argument '--versio`
  - `--ownall --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdpyinfo: unrecognized argument '--help' `
  - `--ownall ` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdpyinfo:  unable to open display ".`
  - `--shim --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdpyinfo: unrecognized argument '--versio`
  - `--shim --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdpyinfo: unrecognized argument '--help' `
  - `--shim ` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xdpyinfo:  unable to open display ".`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xev`
  - `--ownall --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xev: warning: could not set default local`
  - `--ownall --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xev: warning: could not set default local`
  - `--ownall ` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xev: warning: could not set default local`
  - `--shim --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xev: warning: could not set default local`
  - `--shim --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xev: warning: could not set default local`
  - `--shim ` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xev: warning: could not set default local`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xfd`
  - `--ownall --version` → rc=1 `Error: Can't open display: `
  - `--ownall --help` → rc=1 `Error: Can't open display: `
  - `--ownall ` → rc=1 `Error: Can't open display: `
  - `--shim --version` → rc=1 `Error: Can't open display: `
  - `--shim --help` → rc=1 `Error: Can't open display: `
  - `--shim ` → rc=1 `Error: Can't open display: `
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xkill`
  - `--ownall --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xkill: unrecognized argument --version  u`
  - `--ownall --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xkill: unrecognized argument --help  usag`
  - `--ownall ` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xkill:  unable to open display "`
  - `--shim --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xkill: unrecognized argument --version  u`
  - `--shim --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xkill: unrecognized argument --help  usag`
  - `--shim ` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xkill:  unable to open display "`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsatoms`
  - `--ownall --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsatoms: unrecognized argument --version`
  - `--ownall --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsatoms: unrecognized argument --help  u`
  - `--ownall ` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsatoms:  unable to open display "`
  - `--shim --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsatoms: unrecognized argument --version`
  - `--shim --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsatoms: unrecognized argument --help  u`
  - `--shim ` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsatoms:  unable to open display "`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsclients`
  - `--ownall --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsclients: unrecognized argument --versi`
  - `--ownall --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsclients: unrecognized argument --help `
  - `--ownall ` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsclients:  unable to open display " `
  - `--shim --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsclients: unrecognized argument --versi`
  - `--shim --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsclients: unrecognized argument --help `
  - `--shim ` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsclients:  unable to open display " `
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsfonts`
  - `--ownall --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsfonts:  unable to open display ''`
  - `--ownall --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsfonts:  unable to open display ''`
  - `--ownall ` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsfonts:  unable to open display ''`
  - `--shim --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsfonts:  unable to open display ''`
  - `--shim --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsfonts:  unable to open display ''`
  - `--shim ` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xlsfonts:  unable to open display ''`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xprop`
  - `--ownall --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xprop:  unable to open display ''`
  - `--ownall --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xprop:  unable to open display ''`
  - `--ownall ` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xprop:  unable to open display ''`
  - `--shim --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xprop:  unable to open display ''`
  - `--shim --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xprop:  unable to open display ''`
  - `--shim ` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xprop:  unable to open display ''`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xwininfo`
  - `--ownall --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xwininfo: can not set locale properly /da`
  - `--ownall --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xwininfo: can not set locale properly /da`
  - `--ownall ` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xwininfo: can not set locale properly /da`
  - `--shim --version` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xwininfo: can not set locale properly /da`
  - `--shim --help` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xwininfo: can not set locale properly /da`
  - `--shim ` → rc=1 `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/xwininfo: can not set locale properly /da`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/dropbear/dropbearconvert`
  - `--ownall --version` → rc=1 `All arguments must be specified Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dr`
  - `--ownall --help` → rc=1 `All arguments must be specified Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dr`
  - `--ownall ` → rc=1 `All arguments must be specified Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dr`
  - `--shim --version` → rc=1 `All arguments must be specified Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dr`
  - `--shim --help` → rc=1 `All arguments must be specified Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dr`
  - `--shim ` → rc=1 `All arguments must be specified Usage: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/dr`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/jvm/java-17-openjdk-arm64/bin/jfr`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/jvm/java-17-openjdk-arm64/bin/jimage`
  - `--ownall --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --help` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
- `/data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/jvm/java-17-openjdk-arm64/lib/jexec`
  - `--ownall --version` → rc=1 `invalid path: No such file or directory`
  - `--ownall --help` → rc=1 `invalid path: No such file or directory`
  - `--ownall ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`
  - `--shim --version` → rc=1 `invalid path: No such file or directory`
  - `--shim --help` → rc=1 `invalid path: No such file or directory`
  - `--shim ` → rc=2 `Error: could not find libjava.so Error: Could not find Java SE Runtime Environment.`