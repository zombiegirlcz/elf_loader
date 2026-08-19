# Parrot Linux ELF Loader — Magisk module

Runs parrot / Debian / generic **aarch64 Linux** binaries on Android **without
proot, qemu or a full chroot**. The kernel is shared; the parrot `libc` and all
dependencies are loaded and relocated by the bundled own-loading glibc loader
(`/system/bin/elf_loader`), which emulates just enough of the glibc
`ld.so`-private state (`_rtld_global`, `_rtld_global_ro`, fake link-maps,
`_dl_find_dso_for_object`) that own-loaded libc and the backported
`_dl_find_object` machinery work.

## What gets installed

| path | purpose |
|------|---------|
| `/system/bin/elf_loader` | own-loading glibc loader (aarch64 PIE) |
| `/system/bin/parrot` | run any parrot binary: `parrot <cmd> [args...]` |
| `/system/bin/parrot-sh` | interactive parrot shell (`parrot-sh`) |
| `/system/bin/parrot-fix-exec` | enable fork/exec on verity devices |
| `/system/bin/patchelf_interp` | PT_INTERP rewrite tool used by the above |
| `/lib/ld-linux-aarch64.so.1` | experimental interpreter bridge (bind-mount) so `fork/exec` of parrot binaries from inside a parrot shell also works |
| `/data/adb/parrot_root` | config: path to the parrot rootfs |

## Usage

```
adb shell
su
parrot ls -la /etc
parrot sed -n 2p /etc/passwd
parrot-sh            # interactive bash from the rootfs
```

## Rootfs

The module does **not** bundle a rootfs. Set the rootfs path:

```
echo /path/to/parrot/rootfs > /data/adb/parrot_root
```

During install, `customize.sh` auto-detects an existing app-provided rootfs at
`/data/user/0/com.linux_core/files/nh/distro/parrot` and writes it to
`/data/adb/parrot_root`. Otherwise it defaults to `/data/adb/parrot`
(place any Debian/parrot aarch64 rootfs there).

Requirements on the rootfs:
- `usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1`, `libc.so.6`, `libdl.so.2`
- `bin/` + `usr/bin/` executables (Debian layout)

## How it works

`parrot` resolves `<cmd>` under the rootfs, then execs:

```
rootfs/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 \
    --library-path rootfs/usr/lib/aarch64-linux-gnu \
    /system/bin/elf_loader --ownall rootfs/bin/<cmd> ...
```

`elf_loader` maps the rootfs `libc` + deps, relocates them, patches the parrot
allocator onto its own private mmap heap (so it never desyncs the process
`brk`), builds a glibc-shaped TLS region (handles negative TLS offsets), and
switches `TP` before jumping to `main`.

## fork/exec (works on this device)

Inside a parrot shell, `fork/exec` of another parrot binary is handled by the
kernel, which loads the interpreter embedded in the binary. Android's root
filesystem is read-only (dm-verity), so the kernel cannot open the default
interpreter path `/lib/ld-linux-aarch64.so.1`.

The module ships a tiny static (libc-free, raw-syscall) interpreter bridge at
`/system/lib/ld-linux-aarch64.so.1` and a patching tool. Two deployment steps:

1. `parrot-fix-exec` copies the bridge to a writable path
   (`/data/local/tmp/ldl`) and rewrites the `PT_INTERP` of every rootfs ELF to
   that path (backups saved as `<file>.interp.bak`; revert with
   `parrot-fix-exec -r`).  The bridge reads `/data/adb/parrot_root` (rootfs
   path + loader path), recovers the real target path from `AT_EXECFN` of the
   auxiliary vector, and re-execs through the real parrot `ld.so` + `elf_loader`
   (own-loading glibc: private mmap heap, TLS fix, fake link-maps).

```
su
parrot-fix-exec        # enable fork/exec
parrot-fix-exec -r     # revert
```

Validated on a 4.14-perf Android kernel: bash, sed, wc, grep all run via the
bridge with correct output.  NOTE: the patched rootfs no longer works inside
a plain Linux chroot.

Without `parrot-fix-exec`, use `parrot <cmd>` to launch each program directly
(no `/lib` bridge needed).

## Building

```
./build.sh        # needs aarch64-linux-gnu-gcc + zip
# -> parrot_elf_loader.zip (flash in Magisk Manager or recovery)
```

## Caveats

- aarch64 only (matches the own-loader build).
- Device kernel must be capable of the glibc syscalls used (tested on
  4.14-perf).
- Debug output goes to stderr and is quiet on success.