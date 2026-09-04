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
