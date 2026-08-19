/* _start for the minimal static interpreter bridge (see interp_wrapper.c).
 * Linux passes argc/argv on the stack: sp -> argc, sp+8 -> argv[0].
 * entry(argc, argv) returns the exit code, which we hand to the exit syscall.
 */
.global _start
.type _start, %function
_start:
    ldr x0, [sp]          /* argc = *(sp) */
    add x1, sp, #8        /* argv = sp + 8 */
    bl entry
    mov x8, #93           /* exit */
    svc #0
.size _start, .-_start
.section .note.GNU-stack, "", %progbits