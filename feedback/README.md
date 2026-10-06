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
