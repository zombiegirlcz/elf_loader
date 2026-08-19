.text
.globl jump_to_entry
.type jump_to_entry, %function
/* void jump_to_entry(void *entry, void *rsp, uintptr_t new_tp, uintptr_t old_tp) */
jump_to_entry:
    mov x6, x0          /* save entry point */
    mov x7, x1          /* save stack pointer */
    mov x9, x3          /* save old_tp */
    mov x10, x30        /* save caller return address */
    cbz x2, .Lno_switch
    msr tpidr_el0, x2   /* switch to the exe's TLS TP at the very last instant */
.Lno_switch:
    mov x0, #0          /* rtld_fini / main-arg regs: x0 = 0 as at exec */
    mov x2, #0
    mov x3, #0
    mov x4, #0
    mov x5, #0
    mov sp, x7          /* point stack at argc */
    blr x6
    cbz x9, .Lno_restore
    msr tpidr_el0, x9   /* restore host TP before returning to the loader */
.Lno_restore:
    mov x30, x10
    ret
.size jump_to_entry, .-jump_to_entry

.globl lazy_plt_stub
.type lazy_plt_stub, %function
/* Entered with x16 = &GOT[n] (aarch64 PLT protocol). Resolve the slot
   through elf_lazy_resolve(), write the real address into the GOT slot,
   preserve all caller-saved + callee-saved registers, then jump to target. */
lazy_plt_stub:
    stp x29, x30, [sp, #-16]!
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x27, x28, [sp, #-16]!
    stp x0, x1, [sp, #-16]!
    stp x2, x3, [sp, #-16]!
    stp x4, x5, [sp, #-16]!
    stp x6, x7, [sp, #-16]!
    mov x0, x16             /* arg0 = &GOT[n] */
    adrp x16, elf_lazy_resolve
    add x16, x16, :lo12:elf_lazy_resolve
    blr x16                 /* x0 = resolved target */
    mov x16, x0
    ldp x6, x7, [sp], #16
    ldp x4, x5, [sp], #16
    ldp x2, x3, [sp], #16
    ldp x0, x1, [sp], #16
    ldp x27, x28, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ldp x29, x30, [sp], #16
    br x16
.size lazy_plt_stub, .-lazy_plt_stub

.globl tlsdesc_return
.type tlsdesc_return, %function
/* aarch64 TLSDESC resolver for static TLS: x0 = &descriptor, return offset */
tlsdesc_return:
    ldr x0, [x0, #8]
    ret
.size tlsdesc_return, .-tlsdesc_return

.section .note.GNU-stack, "", %progbits