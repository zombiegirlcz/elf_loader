.global _start
.type _start, %function
_start:
    mov x0, sp
    add x1, sp, #8
    bl go
    mov x8, #93
    svc #0
.size _start, .-_start
.section .note.GNU-stack, "", %progbits