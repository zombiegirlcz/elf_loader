# Device feedback

Reports from real devices: what runs, what crashes, on which hardware.
Pull requests into `dev` that **only** add or change files in this directory
are merged automatically (workflow `feedback-automerge`). A PR that touches
anything outside `feedback/` waits for a manual review.

## How to add a report

1. Fork the repo, create a branch from `dev`.
2. Add one file: `feedback/<device>-<yyyy-mm-dd>.md` (e.g.
   `feedback/pixel8-2026-10-06.md`). Use plain Markdown, no symlinks.
3. Open a pull request against **`dev`** (not `master`).

## Generate the report automatically

With the shell helpers loaded (`eval "$(elf_loader init zsh)"` or `init bash`):

```sh
lxfb -m "crashes right after start" claude
# ── exit=139 (SIGSEGV)  report: ~/.cache/lx/feedback/<device>-<date>-claude-<time>.md
```

`lxfb` runs the command with full loader debug logging and writes a ready
Markdown report: date, device / SoC, Android version, kernel, page size,
loader version, rootfs OS + glibc version, the command, exit code / signal,
crash lines, the full log and the loader's `diag.<pid>.txt`. **Read it before
sending** (it contains paths and the command's arguments), then copy the `.md`
into `feedback/` in your fork. For a working binary, a short manual report
using the template below is enough.

## Template

```markdown
- Device / SoC:
- Android version, kernel (`uname -srm`):
- Page size (`getconf PAGESIZE`):
- Rooted: yes / no
- elf_loader version (`elf_loader --version`):
- Rootfs (distro, glibc version):

| binary | command | result |
|---|---|---|
| python3 | `lx python3 -c 'print(1)'` | ✅ |
| node | `lx node -e 'console.log(42)'` | ❌ SIGSEGV (log below) |

Logs / notes:
```
