#!/system/bin/sh
# Shim, ktery se tvari jako `elf_loader --ownall`, ale spusti program
# v NATIVNIM modu (prompt 12). Slouzi k prepnutí test-all.sh:
#
#   ashell -c "… L=$D/usr/bin/lxnative-ownall.sh \
#              $D/usr/bin/elf_loader --ownall $R/bin/bash test-all.sh basic text …"
#
# Harness sam zustava pod loaderem - vnoreny tracer by kolidoval s vnejsim
# (jeden tracee ma jen jednoho tracera, PTRACE_O_TRACEFORK vnejsiho by
# vnitrnimu lxnative sebral dite a jeho PTRACE_TRACEME by selhal na EPERM).
# Prepina se tedy jen jednotlive testy, ne cely beh.
#
# Preklada jen to, co harness skutecne pouziva: `--ownall`, `--shim` a
# `--argv0 X`. Ostatni prepinace loaderu v nativnim modu neexistuji a shim
# je zahodi - to je zamerne, aby bylo v logu videt, ze test meri neco jineho.
set -u

FILES=/data/user/0/com.linux_core/files
RUN="${LXN_RUN:-$FILES/tmp/p12/lxnative-run.sh}"
ROOT="${ROOTFS:-$FILES/nh/distro/parrot}"
ARGV0=""

while [ $# -gt 0 ]; do
    case "$1" in
        --ownall|--shim) shift ;;
        --argv0) ARGV0="$2"; shift 2 ;;
        --*) shift ;;
        *) break ;;
    esac
done

[ $# -ge 1 ] || { echo "lxnative-ownall: chybi program" >&2; exit 2; }
[ -n "$ARGV0" ] || ARGV0=""

exec "$RUN" -R "$ROOT" "$@"
