# elf_loader — Running Linux (glibc) binaries on Android without root

`elf_loader` is an own-loading ELF64 loader that runs **glibc/Parrot binaries
directly under Android (bionic)** — without proot, without chroot, without
QEMU. It loads the guest `libc.so.6` and its dependencies into a private
scope, resolves relocations (including `R_AARCH64_IRELATIVE`/ifunc), builds
the stack and auxv, and jumps to the guest entry point.

The same kernel (Android), two userspaces: the bionic host and a glibc guest
from a rootfs (e.g. Parrot Security / Debian).

## Quick start

Everything is driven by environment variables — **no hard-coded paths**, so it
works for any app and any rootfs:

```sh
export D=$HOME                          # app files dir (= HOME)
export ROOTFS=$D/nh/distro/parrot       # guest rootfs
export R=$ROOTFS
export L=$D/usr/bin/elf_loader          # the loader binary

# one eval gives you the whole `lx` environment (lx, lxwhich, help, ...)
eval "$($L init zsh)"                   # or: eval "$($L init bash)"

lx cat /etc/os-release                  # runs the guest cat
lx python3 -c 'import sys; print(sys.version)'
lx gcc hello.c -o hello && lx ./hello   # compile + run inside the guest
```

## What `init zsh|bash` provides

`elf_loader init zsh` (and `init bash`) prints `eval`-able shell code — the
same pattern as `starship init zsh` or `zoxide init zsh`. Everything is derived
from `$HOME`/`$ROOTFS`:

| variable | meaning | default |
|---|---|---|
| `D` | app files dir | `$HOME` |
| `ROOTFS` | guest rootfs | `$D/nh/distro/parrot` |
| `R` | alias for `ROOTFS` | `$ROOTFS` |
| `L` | loader binary | `$D/usr/bin/elf_loader` |
| `LX_LOG` | log directory | `$HOME/.cache/lx` |
| `LOCPATH` | guest locale dir (only if it exists) | `$ROOTFS/usr/lib/locale` |
| `LC_ALL` | locale | `C.UTF-8` |

Overridable from outside: `ROOTFS=/other/rootfs zsh`.

### Shell helpers

`init zsh` / `init bash` also install these functions:

| command | description |
|---|---|
| `lx <cmd> [args]` | run a guest binary under the loader (searches `LX_PATH`) |
| `lxwhich <cmd>` | show the full path of a guest binary |
| `lxq <cmd>` | like `lx`, but filters `[MMAP]` noise on stderr |
| `lxdbg VAR=1 <cmd>` | run with a loader debug variable |
| `lxhelper <.so> <cmd>` | run with a custom glibc helper library |
| `lxlog <cmd>` | run with output to a log, show the tail and exit code |
| `lxdiag [-c]` | loader `diag.txt` (SIGSYS/INIT traces); `-c` clears it |
| `lxinfo` | loader size/date, rootfs, repo branch |
| `lxfault <cmd>` | print only crash lines (FAULT, pc in, SIGSYS) |
| `lxtest` | regression set (echo, bash, python, node, bun, tmux, ...) |
| `help [topic]` (alias `lxhelp`) | built-in help with examples |
| `<unknown command>` | auto-found in the rootfs and run via the loader |

`lx` passes a guest `PATH` (paths inside the rootfs), so tools that spawn
helpers by name (e.g. `gcc` looking for `ld`/`as`) work.

## CLI

```
elf_loader --help | -h
elf_loader --version | -V
elf_loader --check <file>
elf_loader [--lazy] --run   <elf> [args..]     host-loader mode (host libc)
elf_loader [--lazy] --own   <elf> <shared.so>  own-load one shared module
elf_loader [--lazy] --ownall <elf> [args..]    own-load all deps (guest glibc)
elf_loader [--lazy] --shim  <elf> [args..]     F2 path-translation shim
elf_loader init zsh|bash                        print eval-able shell env
elf_loader <elf>                                introspect (no execution)
```

## Environment variables

| variable | meaning |
|---|---|
| `ROOTFS` | guest rootfs for `--ownall`/`--shim` |
| `ELF_LOADER` | loader binary path (default `/proc/self/exe`) |
| `ELF_DEBUG` | enable load-progress trace |
| `F2_FILTER` | enable the seccomp path filter in `--shim` mode (off by default) |
| `ELF_LOADER_PRELOAD` | guest-only `LD_PRELOAD` (glibc libs for the guest) |
| `ELF_LOADER_HELPER` | custom glibc helper libraries (colon-separated) |
| `ELF_LOADER_NO_COMPAT` | skip the seccomp compat filter |
| `ELF_LOADER_GOMODE=0` | disable Go-binary mode |
| `ELF_LOADER_DIAG=1` | enable diagnostics |

## Demo

`tools/demo-env.zsh` is a scripted demo for a screen recording. It starts in an
**empty environment** (`env -i`) and progressively builds everything:

```sh
env -i HOME=/data/user/0/com.linux_core/files \
    /data/user/0/com.linux_core/files/usr/bin/zsh \
    /data/user/0/com.linux_core/files/tmp/demo-env.zsh
```

Steps: `env -i` proof → set `D/ROOTFS/R/L` → one `eval "$($L init zsh)"` →
`elf_loader --help` → `help` (lxhelp) → starship → python → uv → node → pi →
hand over to an interactive zsh (starship prompt).

## Build

Cross-compiled with the Android NDK via GitHub Actions (workflow
`build-elf-loader`):

```sh
tools/gh_build_deploy.sh          # build HEAD, download, deploy to device
tools/gh_build_deploy.sh --push   # push first
```

Or locally with the NDK:

```sh
TC=/opt/android-ndk-r28/toolchains/llvm/prebuilt/linux-x86_64/bin
$TC/aarch64-linux-android24-clang -Wall -Wextra -g -O0 -std=c11 \
    src/main.c src/elf_loader.c src/ldso_tls.c src/entry.S -ldl -o elf_loader_ndk
```

The result is a bionic binary with `PT_INTERP /system/bin/linker64`.

## More

See `postup.md` for the full development diary (certification, seccomp
emulation, history, individual fixes).