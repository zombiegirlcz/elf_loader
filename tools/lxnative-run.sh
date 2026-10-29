#!/system/bin/sh
# lxnative-run - prepinac "nativni mod" (prompt 12, prototyp).
#
# Spusti guest program pres NATIVNI ld.so rootfs, misto own-loadu loaderem.
# Nemeni nic na defaultu - je to samostatny nastroj, ktery se da pustit
# vedle `elf_loader` a vedle prootu.
#
# Skladba:
#   lxnative        - ptrace supervizor, pohlcuje SIGSYS z app seccomp profilu
#                     (set_robust_list 99 a rseq 293 v glibc TLS initu).
#   liblx-audit.so  - rtld-audit knihovna, preklad cest + exec shim.
#   ld.so           - interpret rootfs, spusteny jako program.
#
# Pouziti:
#   tools/lxnative-run.sh [-R rootfs] [-v] <prog> [args…]
#     -R  rootfs (default parrot); `kali` je druhy ověřený (glibc 2.43)
#     -v  trasovat pohlcene SIGSYS i prelozene cesty
#
# Build obou casti (rootfs gcc, NE bionic - viz hlavicky zdrojaku):
#   tools/lxnative-build.sh
set -u

FILES=/data/user/0/com.linux_core/files
BIN="${LXN_BIN:-$FILES/tmp/p12}"
ROOT="$FILES/nh/distro/parrot"
VERBOSE=""

while [ $# -gt 0 ]; do
    case "$1" in
        -R) case "$2" in
                /*) ROOT="$2" ;;
                *)  ROOT="$FILES/nh/distro/$2" ;;
            esac
            shift 2 ;;
        -v) VERBOSE=-v; LX_AUDIT_TRACE=1; export LX_AUDIT_TRACE; shift ;;
        --) shift; break ;;
        *)  break ;;
    esac
done

[ $# -ge 1 ] || { echo "pouziti: lxnative-run.sh [-R rootfs] [-v] <prog> [args…]" >&2; exit 2; }
[ -d "$ROOT" ] || { echo "neni rootfs: $ROOT" >&2; exit 2; }

LD="$ROOT/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
LP="$ROOT/lib/aarch64-linux-gnu:$ROOT/usr/lib/aarch64-linux-gnu"
[ -e "$LD" ] || { echo "neni ld.so: $LD" >&2; exit 2; }

# Guest PATH, ne hostovy. Bez toho shell v guestu najde `ls` v /system/bin
# a spusti Androidi toybox - TISE, s jinym chovanim i jinym /etc.
PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin
export PATH
export LX_ROOTFS="$ROOT"
export LX_LDSO="$LD"
export LX_LIBPATH="$LP"
export LX_AUDIT="$BIN/liblx-audit.so"

# Absolutni cestu hledame NEJDRIV v rootfs, az potom na hostu. Opacne poradi
# je past: Android ma `/bin` -> `/system/bin`, takze `[ -e /bin/ls ]` na hostu
# uspeje a pres glibc ld.so by se spustil Androidi toybox - ten pak hlasi
# "libcrypto.so: cannot open shared object file" (bionic jmena bez verze).
prog="$1"; shift
case "$prog" in
    /*) [ -e "$ROOT$prog" ] && prog="$ROOT$prog" ;;
esac

# Aby `/proc/self/exe` ukazoval na program, ne na ld.so.
export LX_PROG="$prog"

exec "$BIN/lxnative" $VERBOSE "$LD" --library-path "$LP" \
     --argv0 "$prog" --audit "$BIN/liblx-audit.so" "$prog" "$@"
