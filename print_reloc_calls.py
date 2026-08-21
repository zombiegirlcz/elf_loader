# Let us write a quick tool or script to patch elf_loader.c with prints at the start of elf_relocate and see if it gets called multiple times or with different values.
# Wait, we already have print statements!
# In the log:
#   [dbg] mapped libc.so.6 -> 0x71d3f6a000 (total 0x1bf000)
#   [dbg] /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so.6 TLS blk=0x725802a000 TP=...
#   [WARN] Unresolved RELA JUMP_SLOT: __rseq_offset in libc.so.6
#   [WARN] Unresolved RELA JUMP_SLOT: __rseq_size in libc.so.6
#   [WARN] Unresolved RELA JUMP_SLOT: __libc_stack_end in libc.so.6
#   [dbg] patching libc.so.6 sbrk ...
#   [dbg] libc pre-init mp_ bytes: ...
#   [+] running libc.so.6 init_array[0] @ 0xe3a7ef6080
# Where did the warnings:
#   [WARN] Unresolved RELA JUMP_SLOT: __rseq_offset in libc.so.6
# come from? They come from elf_relocate!
# But wait! Why did it NOT print:
#   [DBG] libc.so.6: processed 1188 jmp_rela entries out of 480 bytes
#   [+] relocated 1188 entries
# in the current run?
# Ah!
# Let us look at the log output from the current run again:
#   [WARN] Unresolved RELA JUMP_SLOT: __rseq_offset in libc.so.6
#   [WARN] Unresolved RELA JUMP_SLOT: __rseq_size in libc.so.6
#   [WARN] Unresolved RELA JUMP_SLOT: __libc_stack_end in libc.so.6
#   [dbg] patching libc.so.6 sbrk ...
# Wait! Did elf_relocate exit or crash inside the relocation loop?
# No! If it crashed, it would have segfaulted *during* elf_relocate, not at "[+] running libc.so.6 init_array[0] @ 0xe3a7ef6080"!
# But wait, why did it not print:
#   [DBG] libc.so.6: processed ...
# If it finished elf_relocate, it MUST have printed "[+] relocated ... entries"!
# Let us look at the log:
#   [WARN] Unresolved RELA JUMP_SLOT: __rseq_offset in libc.so.6
#   [WARN] Unresolved RELA JUMP_SLOT: __rseq_size in libc.so.6
#   [WARN] Unresolved RELA JUMP_SLOT: __libc_stack_end in libc.so.6
#   [dbg] patching libc.so.6 sbrk @0x71d4055500 -> 0x5722dce600
# Wait! "patching libc.so.6 sbrk" is called *after* elf_relocate(m) in elf_load_shared:
#   elf_relocate(m);
#   patch_module_heap_syms(m);
# But we did not see "[DBG] libc.so.6: processed ..." or "[+] relocated ... entries" in the log!
# Wait! Why?
# Let us check stdout vs stderr!
# printf goes to stdout. fprintf(stderr) goes to stderr.
# In the log:
#   [dbg] patching libc.so.6 sbrk  <-- this is fprintf(stderr)!
#   [+] running libc.so.6 init_array[0]  <-- this is fprintf(stderr)!
# Wait, printf goes to stdout, which is buffered!
# If the program segfaulted at init_array[0], the stdout buffer (which had "[DBG]" and "[+] relocated") was NEVER FLUSHED to the shell!
# So the messages *were* printed to the stdout buffer, but the buffer was lost on segfault!
# That explains why we did not see them in the log!
#
# But why is init_array[0] double relocated?
# If elf_relocate(m) only ran once, then how did it get relocated twice?
# Let us check: did apply_relr get called twice, or did both DT_RELR and DT_RELA relocate it?
# We verified that 0x1ad0b0 is NOT in DT_RELA. It is only in DT_RELR.
# If apply_relr only ran once, and it is not in DT_RELA, then init_array[0] should be relocated exactly once!
# Wait, what if Bionic/host dynamic linker relocated it? No.
# What if apply_relr is called TWICE?
# Let us check if apply_relr is called outside elf_relocate.
# We grepped "apply_relr" and found:
#   static void apply_relr(elf_object_t *obj)
#   apply_relr(obj); (inside elf_relocate)
# So apply_relr is only called inside elf_relocate.
#
# Then did elf_relocate run twice?
# Let us trace if libc.so.6 is in scope->mods twice!
# In elf_load_shared:
#   load_module_needed(m, scope);
#   elf_scope_add(scope, m);
#   elf_relocate(m);
# Wait!
# When libc.so.6 is being loaded, it calls elf_relocate(m) (first time).
# Then after all modules are loaded, does the caller call elf_relocate(m) AGAIN?
# Let us look at run_ownall in main.c:
#   elf_object_t *obj = elf_load(path); (which own-loads dependencies like libc.so.6)
#   g_libc_base = ...
#   if (elf_relocate(obj) != 0) { ... }
# Wait! Does run_ownall call elf_relocate for each dependency?
# Let us check main.c line 150-160:
#   if (elf_relocate(obj) != 0) { ... }
# Wait, it only calls elf_relocate(obj) (the EXE)!
# But wait! Does elf_relocate(obj) call elf_relocate on its dependencies?
# Let us check elf_relocate implementation!
