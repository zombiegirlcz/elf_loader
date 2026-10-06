# How elf_loader works

elf_loader runs glibc (Linux) binaries directly on Android (bionic), in the
same process, without root, proot or chroot. Five things make that possible.

## 1. Own-loading, not `dlopen`

The loader maps each ELF64 `PT_LOAD` segment itself, applies relocations, and
resolves `DT_NEEDED` into a private scope — it does **not** use the host
bionic linker. Both the guest `libc.so.6` and the host bionic libc live in one
process, each with its own symbol table.

## 2. IFUNC / IRELATIVE

glibc resolves `memcpy`, `strlen`, … through `R_AARCH64_IRELATIVE` (ifunc)
relocations at load time. The loader calls the resolver and patches the GOT
with the result (`resolve_jmp_symbol`). Without this, every `memcpy` traps.

## 3. Two allocators, one `brk`

The guest glibc `malloc` and the host bionic `malloc` share the process `brk`.
If either trims the heap, it unmaps the other's live chunks. The loader maps a
private arena at `0x7f00000000` and redirects the guest's `sbrk`/`brk` there,
so the two allocators never touch each other.

## 4. Static TLS + guest thread pointer

glibc expects thread-local storage below `TP`. The loader mmaps a TLS block,
copies the `tcbhead`, and switches `tpidr_el0` to the guest TP only in the
final trampoline (`entry.S`). Module init functions and the guest entry run
under the guest TP; loader internals run under the bionic TP.

## 5. Seccomp compat (app-profile kernel 4.14)

Android's app seccomp profile kills newer syscalls (`clone3`, `close_range`,
…) with `SIGSYS` instead of returning `ENOSYS`. The loader stacks its own
filter that returns `ENOSYS` for those, so glibc falls back to its legacy
paths — plus a `fork` veneer built on `clone` for the same reason.

Everything else (signal handlers, the `SIGSYS` path-translation shim for
`--shim`, the Go-binary mode, helper libraries) builds on these five.

See [`postup.md`](../postup.md) for the full, chronological development diary
with every bug, diagnosis and commit.