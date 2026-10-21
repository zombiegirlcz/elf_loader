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

.text
.globl elf_final_jump
.type elf_final_jump, %function
/* void elf_final_jump(void *sp, void *entry, uintptr_t new_tp,
 *                     void (*run_inits)(void))
 * NORETURN: switch TP, spustit queued module inity (pod parrot TP),
 * pak skoc na exe entry s exe stackem. Pouze asm - zadny compiler-generated
 * stack access po msr. */
elf_final_jump:
    /* x19/x20 = callee-saved: C helper (blr x6) je musi zachovat dle ABI */
    mov x19, x1          /* entry */
    mov x20, x0          /* sp */
    mov x6, x3           /* run_inits helper */
    cbz x2, .Lfj_ns
    msr tpidr_el0, x2
.Lfj_ns:
    /* DIAG: zaloguj realne tpidr_el0 hned po msr (pred inity). */
    stp x6, x30, [sp, #-16]!
    bl elf_diag_post_msr
    ldp x6, x30, [sp], #16
    blr x6              /* elf_run_pending_inits() - C loop, malloc-free */
    /* DIAG: zaloguj realne tpidr_el0 tesne pred skokem do guest entry. */
    stp x6, x30, [sp, #-16]!
    bl elf_diag_pre_entry
    ldp x6, x30, [sp], #16
    mov x0, #0
    mov x1, #0
    mov x2, #0
    mov x3, #0
    mov x4, #0
    mov x5, #0
    mov x29, xzr
    mov x30, xzr
    mov sp, x20
    br x19
.size elf_final_jump, .-elf_final_jump

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
/* ---- Bionic bridge thunky ------------------------------------------------
 * bb_stubs[i]  guest glibc -> bionic (cil bb_targets[i])
 * rb_stubs[i]  bionic -> guest glibc (cil rb_targets[i]; callbacky)
 * Stub: x16 = i, skok do *_common. Spolecny ramec (BB_FRAME, viz
 * bb_frame_t v src/elf_loader.c):
 *   0 stack[14] | 112 x29,x30 | 128 x19,x20 | 144 x0-x7 | 208 x8 | 216 idx
 *   224 mask | 232 tp | 240 q0-q7 | 368 alloc[6] | 416 patch[4] | 448 konec
 * *_pre(frame) (C) vrati cil a do frame->tp zapise cilovy TP (parovani
 * vlaken guest<->bionic), bb_pre navic podmeni Vulkan callbacky.
 * Forward smer blokuje async signaly (raw rt_sigprocmask, x5 = F2_SENTINEL). */
#define BB_FRAME 448

.macro BB_SAVE
    sub sp, sp, #BB_FRAME
    stp x29, x30, [sp, #112]
    add x29, sp, #112
    stp x19, x20, [sp, #128]
    stp x0, x1, [sp, #144]
    stp x2, x3, [sp, #160]
    stp x4, x5, [sp, #176]
    stp x6, x7, [sp, #192]
    stp x8, x16, [sp, #208]
    stp q0, q1, [sp, #240]
    stp q2, q3, [sp, #272]
    stp q4, q5, [sp, #304]
    stp q6, q7, [sp, #336]
    add x11, sp, #BB_FRAME
    ldp x9, x10, [x11, #0]
    stp x9, x10, [sp, #0]
    ldp x9, x10, [x11, #16]
    stp x9, x10, [sp, #16]
    ldp x9, x10, [x11, #32]
    stp x9, x10, [sp, #32]
    ldp x9, x10, [x11, #48]
    stp x9, x10, [sp, #48]
    ldp x9, x10, [x11, #64]
    stp x9, x10, [sp, #64]
    ldp x9, x10, [x11, #80]
    stp x9, x10, [sp, #80]
    ldp x9, x10, [x11, #96]
    stp x9, x10, [sp, #96]
.endm

.macro BB_LOAD_ARGS
    ldp x0, x1, [sp, #144]
    ldp x2, x3, [sp, #160]
    ldp x4, x5, [sp, #176]
    ldp x6, x7, [sp, #192]
    ldr x8, [sp, #208]
    ldp q0, q1, [sp, #240]
    ldp q2, q3, [sp, #272]
    ldp q4, q5, [sp, #304]
    ldp q6, q7, [sp, #336]
.endm

.macro BB_SIGMASK how, set_reg_setup
    \set_reg_setup
    mov x0, #\how
    mov x3, #8
    mov x4, #0
    movz x5, #0xCDEF
    movk x5, #0x90AB, lsl #16
    movk x5, #0x5678, lsl #32
    movk x5, #0x1234, lsl #48
    mov x8, #135
    svc #0
.endm

.text
.balign 16
.globl bb_stubs
.type bb_stubs, %function
bb_stubs:
.set bb_i, 0
.rept 4096
    mov x16, #bb_i
    b bb_common
.set bb_i, bb_i + 1
.endr
.size bb_stubs, .-bb_stubs

.type bb_common, %function
bb_common:
    BB_SAVE
    mov x0, sp
    bl bb_pre                         /* pod guest TP; x0 = cil */
    mov x19, x0
    BB_SIGMASK 0, "adrp x1, bb_async_set; add x1, x1, :lo12:bb_async_set; add x2, sp, #224"
    mrs x20, tpidr_el0                /* guest TP */
    ldr x9, [sp, #232]
    msr tpidr_el0, x9                 /* bionic TP tohoto vlakna */
    BB_LOAD_ARGS
    blr x19
    msr tpidr_el0, x20
    stp x0, x1, [sp, #144]
    stp q0, q1, [sp, #240]
    stp q2, q3, [sp, #272]
    BB_SIGMASK 2, "add x1, sp, #224; mov x2, #0"
    mov x0, sp
    bl bb_post                        /* pod guest TP: vrati docasne patche */
    ldp x0, x1, [sp, #144]
    ldp q0, q1, [sp, #240]
    ldp q2, q3, [sp, #272]
    ldp x19, x20, [sp, #128]
    ldp x29, x30, [sp, #112]
    add sp, sp, #BB_FRAME
    ret
.size bb_common, .-bb_common

.balign 16
.globl rb_stubs
.type rb_stubs, %function
rb_stubs:
.set rb_i, 0
.rept 1024
    mov x16, #rb_i
    b rb_common
.set rb_i, rb_i + 1
.endr
.size rb_stubs, .-rb_stubs

.type rb_common, %function
rb_common:
    BB_SAVE
    mov x0, sp
    bl rb_pre                         /* pod bionic TP; x0 = guest cil */
    mov x19, x0
    mrs x20, tpidr_el0                /* bionic TP */
    ldr x9, [sp, #232]
    msr tpidr_el0, x9                 /* guest TP (parovane vlakno) */
    BB_LOAD_ARGS
    blr x19
    msr tpidr_el0, x20
    ldp x19, x20, [sp, #128]
    ldp x29, x30, [sp, #112]
    add sp, sp, #BB_FRAME
    ret
.size rb_common, .-rb_common


.bss
.balign 8
.globl bb_targets
bb_targets:
    .zero 4096 * 8
.globl rb_targets
rb_targets:
    .zero 1024 * 8
.data
.balign 8
.globl bb_async_set
/* ~(SEGV|BUS|ILL|FPE|TRAP|SYS|ABRT): bity 10,6,3,7,4,30,5 (signo-1) */
bb_async_set:
    .quad ~((1 << 10) | (1 << 6) | (1 << 3) | (1 << 7) | (1 << 4) | (1 << 30) | (1 << 5))
