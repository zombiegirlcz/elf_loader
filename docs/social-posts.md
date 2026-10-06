# Social / launch posts (drafts)

Replace `https://cdn.jsdelivr.net/gh/zombiegirlcz/elf_loader@master/docs/media/demo-env.mp4` with the screen recording link, `https://github.com/zombiegirlcz/elf_loader` with the
published blog post, and `https://github.com/zombiegirlcz/elf_loader` with
`https://github.com/zombiegirlcz/elf_loader` where needed.

---

## Hacker News — Show HN

> **HN is a tough room.** It is skeptical, allergic to marketing, and will
> find the weakest claim in three minutes. Do NOT oversell. Lead with the
> technical reality and the parts that don't work. The people who can actually
> help (and the ones who file good bugs) are exactly the ones who respect a
> project that admits its bugs. Expect, and welcome, hard questions:
> "why not proot?", "why not just use Termux/QEMU?", "what's the perf
> overhead?", "is this just `dlopen` with extra steps?". Have short, honest,
> numbers-or-code answers ready. Never argue; if someone is right, say so.

**Title:**
```
Show HN: Run glibc Linux binaries on Android without root, proot or chroot
```

**URL:** `https://github.com/zombiegirlcz/elf_loader` (the blog post) — or `https://github.com/zombiegirlcz/elf_loader` if you post the repo
directly. Show HN prefers a page you can read; the blog post works best.

**First comment (post it yourself, right after submitting):**
```
Author here. This started as an experiment: can I run a real Debian/Parrot
glibc userspace on a stock, non-rooted Android phone without proot or a chroot?

Short answer: yes, by own-loading the guest libc into the same process as the
bionic host and doing the dynamic linking myself. The interesting bugs were
IFUNC/IRELATIVE resolution, the fact that the guest glibc malloc and the host
bionic malloc share one brk (fixed with a private arena), static TLS + the
thread pointer switch point, and Android's seccomp profile killing clone3 /
close_range with SIGSYS instead of ENOSYS.

It runs python3, uv, gcc (compiling and running C inside the guest), git, gh,
fzf, Node, tmux, and an interactive zsh with a starship prompt. No root, no
namespaces, no ptrace.

What I'd most like feedback on: I only have a couple of devices, so the bugs
left are the ones that appear on other phones. There are issue templates for
bug reports and for "it works" reports — even "this ran fine on my Pixel"
is useful.

Happy to answer anything about the loader.
```

---

## Reddit — r/linux_on_android

**Title:**
```
I run glibc Linux binaries on stock (non-rooted) Android without proot, chroot or QEMU — just an ELF loader
```

**Body:**
```
TL;DR: I wrote an own-loading ELF loader that maps a guest glibc rootfs
(Parrot/Debian) into the same process as Android's bionic libc and jumps into
it. No root, no proot, no namespaces, no ptrace.

Demo (screen recording): https://cdn.jsdelivr.net/gh/zombiegirlcz/elf_loader@master/docs/media/demo-env.mp4

On a stock Android 13 phone (kernel 4.14):
  $ lx cat /etc/os-release
  PRETTY_NAME="Parrot Security 7.4 (echo)"
  $ lx gcc hello.c -o hello && lx ./hello
  hello from glibc, 42

That gcc is real Debian gcc 14.2, compiling and linking inside the guest.

Why not proot? proot intercepts syscalls with ptrace — every syscall goes
through a tracer. Own-loading avoids that entirely; the guest runs natively at
full speed. The trade-off is that you have to implement the dynamic linking
yourself, which is where all the interesting bugs were (IFUNC resolvers, two
malloc's sharing one brk, TLS thread-pointer switching, Android's seccomp
profile SIGSYS-ing clone3/close_range instead of returning ENOSYS...).

What works: coreutils, grep/sed/awk, python3 + uv + venvs, gcc (compiles and
runs), git/gh/glab/fzf, Node (≤22 solid, 26 after a fix), tmux, interactive zsh
with starship.

What doesn't (yet): Node ≥23 had a V8 JSDispatchTable issue (fixing), some
network binaries crash intermittently, 16KB pages (Android 15+) untested.

Repo: https://github.com/zombiegirlcz/elf_loader

I'd really appreciate crash reports and "this worked on my device" reports —
I can only test a couple of devices. There are issue templates for both.
```

---

## Reddit — r/termux

**Title:**
```
Run a glibc rootfs (Debian/Parrot) on Android without proot — own-loading ELF loader
```

**Body:** same as r/linux_on_android, but lead with the proot comparison
(Termux users know proot well) and note it's a non-root alternative.

---

## Reddit — r/unixporn

**Title:**
```
[Android] Full glibc userspace on a non-rooted phone — zsh 5.9 + starship, no proot
```

**Body:**
```
Not a desktop rice, but close enough: an interactive zsh 5.9 with a starship
prompt, running a real Debian/Parrot glibc userspace on a stock Android phone,
no root, no proot.

Video: https://cdn.jsdelivr.net/gh/zombiegirlcz/elf_loader@master/docs/media/demo-env.mp4

It's my own ELF loader (own-loads the guest glibc into the bionic process) +
my shell config. Details and repo: https://github.com/zombiegirlcz/elf_loader
```

---

## X / Twitter thread

```
1/ You can run a real glibc (Debian/Parrot) userspace on a *stock, non-rooted*
   Android phone — no proot, no chroot, no emulator. Just an ELF loader that
   own-loads the guest libc into the same process as bionic.

2/ $ lx cat /etc/os-release
   PRETTY_NAME="Parrot Security 7.4 (echo)"
   $ lx gcc hello.c -o hello && lx ./hello
   hello from glibc, 42
   Real Debian gcc 14 compiling inside the guest. Video: https://cdn.jsdelivr.net/gh/zombiegirlcz/elf_loader@master/docs/media/demo-env.mp4

3/ Why not proot? proot ptrace-intercepts every syscall. Own-loading runs the
   guest natively. Trade-off: you implement the dynamic linking yourself —
   IFUNC resolvers, two mallocs sharing one brk, TLS thread-pointer switching,
   Android's seccomp killing clone3 with SIGSYS instead of ENOSYS.

4/ Works: python3+uv, gcc, git/gh/fzf, Node, tmux, interactive zsh+starship.
   Doesn't (yet): Node≥23 had a V8 issue, some network binaries, 16KB pages.

5/ I can only test a couple of devices — please run something on yours and
   file what happens (crash or clean run). Issue templates for both.
   Repo: https://github.com/zombiegirlcz/elf_loader
```

---

## GitHub Discussions — announcement

**Title:** `elf_loader v0.1 — run glibc binaries on Android without root`

Pin this, and use it as the landing thread for questions. Link the video, the
blog post, and the release.