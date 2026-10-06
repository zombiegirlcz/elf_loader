# Running glibc (Linux) binaries on Android without root, proot or chroot

Android phones run a Linux kernel but a completely different userspace:
bionic libc instead of glibc. So a normal Linux binary — built against glibc,
expecting `/lib/ld-linux-aarch64.so.1` — simply won't start. The usual answers
are proot (ptrace-based syscall rewriting), chroot (needs root), or an
emulator (slow, big).

This project takes a different route: **load the guest glibc into the same
process as the bionic host**, resolve it yourself, and jump into it. No root,
no namespaces, no ptrace. Just an ELF loader that does the linking that the
host linker refuses to do.

## What it does

`elf_loader` maps each `PT_LOAD` of the guest binary and its `DT_NEEDED`
dependencies (a Parrot/Debian rootfs `libc.so.6`, `libm`, `libpthread`, …) into
a private scope, applies relocations, builds the stack + auxv, and calls the
guest entry point. The bionic libc and the glibc guest coexist in one process,
each with its own symbol table.

The result, on a stock, non-rooted Android 13 phone (kernel 4.14):

```
$ uname -srm
Linux 4.14.190-perf aarch64
$ cat /etc/os-release
cat: /etc/os-release: No such file or directory      # it's Android
$ lx cat /etc/os-release
PRETTY_NAME="Parrot Security 7.4 (echo)"             # ...and now it's Debian
$ lx gcc hello.c -o hello && lx ./hello
hello from glibc, 42
```

That `gcc` is a real Debian gcc 14.2 running under the loader, compiling and
linking inside the guest userspace.

## Why proot is not the only answer

proot works by intercepting syscalls with `ptrace`. On Android that has real
costs: every syscall goes through the tracer, namespaces can leak, and the
kernel's app seccomp profile sometimes interacts badly with it. Own-loading
skips all of that: the guest binary runs natively, at full speed, in the same
process as the host.

The price is that *you* have to be the dynamic linker. That's where the
interesting bugs live.

## The five hard parts

1. **IFUNC / IRELATIVE.** glibc picks `memcpy`, `strlen`, etc. at load time via
   ifunc resolvers (`R_AARCH64_IRELATIVE`). You have to call the resolver and
   patch the GOT before any guest code runs — otherwise the first `memcpy`
   traps.

2. **Two allocators, one `brk`.** The guest glibc `malloc` and the host bionic
   `malloc` share the process `brk`. If either trims the heap, it unmaps the
   other's live chunks. The fix is a private heap arena that the guest's
   `sbrk`/`brk` are redirected to, so the two allocators never meet.

3. **Static TLS and the thread pointer.** glibc expects its TLS block *below*
   `TP`. You mmap one, copy the `tcbhead`, and switch `tpidr_el0` to the guest
   TP — but only in the final trampoline, so loader internals keep running on
   the bionic TP. Getting the switch point wrong shows up as a SIGSEGV in V8
   or libstdc++ long before you understand why.

4. **Seccomp compat.** Android's app seccomp profile (kernel 4.14) *kills*
   newer syscalls like `clone3` and `close_range` with `SIGSYS` instead of
   returning `ENOSYS`. glibc expects `ENOSYS` so it can fall back to its legacy
   paths. The loader stacks its own filter that returns `ENOSYS` for those, and
   provides a `fork` veneer built on `clone` for the same reason.

5. **Signal handlers under two libcs.** Both libcs install handlers; both read
   `TPIDR_EL0`. A handler installed under the wrong TP silently doesn't get
   installed, and a crash becomes a bare "Segmentation fault" with no
   information. This is the kind of bug that takes a week and turns out to be
   one `mrs`/`msr` dance.

## What runs today

Verified on a stock Android 13 device, kernel 4.14:

- coreutils, `grep`, `sed`, `awk`, `find`, `diff`, `tar`, …
- `python3` (glibc 2.41), `uv`, virtualenvs
- `gcc` 14 (compiles and runs C code inside the guest)
- `git`, `gh`, `glab`, `git-lfs`, `fzf` (Go binaries, via a dedicated mode)
- Node.js ≤ 22 (LTS), and Node 26 after a recent fix
- `tmux`, an interactive zsh 5.9 with a starship prompt
- Bun (`claude.exe`)

There's a regression suite (`test-all.sh`) that currently reports **160 PASS /
0 FAIL** on the test device.

## What doesn't, yet

Being honest about the rough edges is the point:

- **Node.js ≥ 23** had a `JSDispatchTable` bootstrap nondeterminism in V8 that
  caused intermittent SIGSEGV. Node 22 is rock-solid; Node 26 now starts but is
  still being shaken out across V8 versions.
- Some **network binaries** still crash intermittently under the bionic host.
- **16 KB page size** (Android 15+) is untested — no device.
- An intermittent ~5 % SIGSEGV in helper libraries is under a heap fix.

## Help wanted

This is built and tested against a small number of devices, which is exactly
the problem: the bugs left are the ones that show up on *your* phone, not mine.
So the most useful thing you can do is **run something and file what happens** —
whether it's a crash or a clean run.

There are issue templates for both:
- a structured **bug report** (loader MD5, rootfs, device, mode, command,
  output, and — if you have a guess — your diagnosis),
- an **"it works"** report for a guest binary that ran fine.

Reports of which binaries work are as valuable as crash reports: they map the
territory and let the docs say "verified on X".

## Try it

The loader is cross-compiled with the Android NDK; the repo has a GitHub
Actions workflow and a script that deploys straight to a device over ADB or a
device shell. Everything is environment-variable driven, so it works with any
rootfs:

```sh
export ROOTFS=/path/to/rootfs
export L=/path/to/elf_loader
eval "$($L init zsh)"
lx python3 -c 'import sys; print(sys.version)'
```

Repository: **https://github.com/zombiegirlcz/elf_loader**

Docs: [`README.md`](../README.md), [`docs/how-it-works.md`](how-it-works.md).
The full development diary (every bug, diagnosis and commit) is in
[`postup.md`](../postup.md).