# ELF Loader — Magisk module

Runs aarch64 **glibc (Linux)** binaries from a rootfs directly on Android —
no proot, no qemu, no chroot. The kernel is shared; the guest `libc` and all
its dependencies are loaded and relocated by the bundled own-loading loader
(`/system/bin/elf_loader`). Path translation happens inside the loader, so
nothing is bind-mounted into `/system` or `/lib`.

## What gets installed

| path | purpose |
|------|---------|
| `/system/bin/elf_loader` | own-loading glibc loader (aarch64, bionic NDK build) |
| `/system/bin/zsh` | bundled bionic-native zsh 5.9.2 — a **host** shell |
| `/system/bin/bzsh` | launcher for it (`ZDOTDIR` / `TERMINFO` → `/data/adb/zsh-bionic`) |
| `/system/etc/zsh-bionic/` | terminfo (xterm/screen/tmux/linux/vt100) + `zshrc` |
| `/data/adb/elf_loader_root` | config: path to the glibc rootfs |

Copies under `/data/adb/` (zsh, bzsh, zsh-bionic) exist because the Magisk
`/system` mount is **not visible inside app/isolated namespaces**.

Nothing else is installed. Older releases shipped `parrot`, `parrot-sh`,
`parrot-fix-exec`, `patchelf_interp`, `linuxsh` and an interpreter bridge at
`/lib/ld-linux-aarch64.so.1`; `customize.sh` removes those leftovers on
upgrade.

## Usage

```
bzsh                       # host shell (bionic zsh), no rootfs needed
bzsh -c 'lx ls -la /etc'   # guest binary from the rootfs
```

Inside `bzsh` the whole guest toolkit is available once a rootfs is
configured (`lx`, `lxwhich`, `lxdw`, `elf_loader init zsh|bash`).

## Rootfs

The module does **not** bundle a rootfs. `customize.sh` auto-detects one
(any `/data/{user,data,adb}/*/files/nh/distro/parrot` containing
`usr/lib/aarch64-linux-gnu/libc.so.6`) and writes it to
`/data/adb/elf_loader_root`. Otherwise set it yourself:

```
echo /path/to/rootfs > /data/adb/elf_loader_root
```

On upgrade the old `/data/adb/parrot_root` is migrated automatically.

Requirements on the rootfs:
- `usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1`, `libc.so.6`
- Debian layout (`bin/`, `usr/bin/`)

## Configuration

`zshrc` in the module is deliberately tiny — it only evals the loader:

```zsh
eval "$(elf_loader init zsh)"
```

That single line sets `D` / `ROOTFS` / `L` / `R` / `LX_LOG`, installs the
`lx` / `lxwhich` / `lxdw` helpers and, at the end, sources `~/.elfrc`.

Everything user-facing (starship, zoxide, aliases) belongs in `~/.elfrc`,
**not** in `~/.zshrc` — the rootfs rc is shared between the host and the
guest world. Generate a ready-made one:

```
elf_loader init example   # print template
elf_loader init install   # write ${ELF_RC:-$HOME/.elfrc}
```

Skip it with `LX_NO_RC=1`.

## Building

```
./build.sh        # -> elf_loader.zip
```

`build.sh` stages the prebuilt bionic `elf_loader`, the bundled `zsh`/`bzsh`,
terminfo + `zshrc`, the module scripts and `META-INF`. It needs no
cross-compiler: both binaries are prebuilt into `system/bin/` (the loader is
produced by this repo's GitHub Actions NDK build).

## Caveats

- aarch64 only.
- Device kernel must support the glibc syscalls used (tested on 4.14-perf).
- `hardlink(2)` is denied by SELinux in the app domain — tools that rely on it
  (`uv` cache persistence, `dpkg` hardlink backups) need `LX_L2S=1` or a
  workaround.