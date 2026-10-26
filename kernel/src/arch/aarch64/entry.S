.section .text.entry, "ax"
.align 4
.global _start
.type _start, %function
_start:
    mov x9, sp
    and x9, x9, #0xfffffffffffffff0
// Limine guarantees 64 KiB below SP; split it into normal and exception stacks.
    sub x10, x9, #32768
    adrp x12, aarch64_boot_stack_top
    add x12, x12, :lo12:aarch64_boot_stack_top
    str x9, [x12]
    adrp x12, aarch64_boot_normal_stack_top
    add x12, x12, :lo12:aarch64_boot_normal_stack_top
    str x10, [x12]
    mrs x11, CurrentEL
    cmp x11, #4
    b.eq 1f
    cmp x11, #8
    b.eq 2f
    b aarch64_exception_halt
1:
    msr SPSel, #1
    mov sp, x9
    msr SPSel, #0
    mov sp, x10
    b 3f
2:
    msr SPSel, #1
    mov sp, x9
    msr SPSel, #0
    mov sp, x10
3:
    isb
    bl aarch64_kernel_main
1:
    wfi
    b 1b
.size _start, . - _start

.section .bss.stack_metadata, "aw", %nobits
.balign 8
.global aarch64_boot_stack_top
aarch64_boot_stack_top:
    .skip 8
.global aarch64_boot_normal_stack_top
aarch64_boot_normal_stack_top:
    .skip 8

.section .text.vectors, "ax"
.balign 2048
.global aarch64_exception_vectors
.type aarch64_exception_vectors, %function
aarch64_exception_vectors:
.macro VECTOR_SLOT target
    b \target
    .space 124
.endm
    VECTOR_SLOT aarch64_vector_0
    VECTOR_SLOT aarch64_vector_1
    VECTOR_SLOT aarch64_vector_2
    VECTOR_SLOT aarch64_vector_3
    VECTOR_SLOT aarch64_vector_4
    VECTOR_SLOT aarch64_vector_5
    VECTOR_SLOT aarch64_vector_6
    VECTOR_SLOT aarch64_vector_7
    VECTOR_SLOT aarch64_vector_8
    VECTOR_SLOT aarch64_vector_9
    VECTOR_SLOT aarch64_vector_10
    VECTOR_SLOT aarch64_vector_11
    VECTOR_SLOT aarch64_vector_12
    VECTOR_SLOT aarch64_vector_13
    VECTOR_SLOT aarch64_vector_14
    VECTOR_SLOT aarch64_vector_15
.size aarch64_exception_vectors, . - aarch64_exception_vectors

.section .text.exceptions, "ax"
.macro SAVE_EXCEPTION index
    sub sp, sp, #320
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mov x9, #\index
    str x9, [sp, #296]
    mrs x9, CurrentEL
    lsr x9, x9, #2
    str x9, [sp, #288]
    cmp x9, #1
    b.eq 1f
    cmp x9, #2
    b.eq 2f
    b aarch64_exception_halt
1:
    mrs x10, ELR_EL1
    mrs x11, SPSR_EL1
    str x10, [sp, #256]
    str x11, [sp, #264]
    .if ((\index % 4) == 0)
        mrs x9, ESR_EL1
        str x9, [sp, #248]
        mov x12, #1
        str x12, [sp, #304]
        b 3f
    .endif
    str xzr, [sp, #248]
    str xzr, [sp, #304]
    b 3f
2:
    mrs x10, ELR_EL2
    mrs x11, SPSR_EL2
    str x10, [sp, #256]
    str x11, [sp, #264]
    .if ((\index % 4) == 0)
        mrs x9, ESR_EL2
        str x9, [sp, #248]
        mov x12, #1
        str x12, [sp, #304]
        b 3f
    .endif
    str xzr, [sp, #248]
    str xzr, [sp, #304]
3:
    str xzr, [sp, #272]
    str xzr, [sp, #280]
    ldr x9, [sp, #304]
    cbz x9, 5f
    ldr x9, [sp, #248]
    ubfx x12, x9, #26, #6
    cmp x12, #0x20
    b.lo 5f
    cmp x12, #0x26
    b.hs 5f
    ldr x9, [sp, #288]
    cmp x9, #1
    b.eq 4f
    mrs x10, FAR_EL2
    b 6f
4:
    mrs x10, FAR_EL1
6:
    str x10, [sp, #272]
    mov x10, #1
    str x10, [sp, #280]
5:
    mov x0, sp
    mov x19, sp
    bl aarch64_exception_dispatch
    cbz w0, aarch64_exception_halt
    ldr x9, [x19, #288]
    ldr x10, [x19, #256]
    cmp x9, #1
    b.eq 7f
    msr ELR_EL2, x10
    b 8f
7:
    msr ELR_EL1, x10
8:
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #320
    eret
.endm

.macro VECTOR_HANDLER index
    .global aarch64_vector_\index
aarch64_vector_\index:
    SAVE_EXCEPTION \index
.endm

VECTOR_HANDLER 0
VECTOR_HANDLER 1
VECTOR_HANDLER 2
VECTOR_HANDLER 3
VECTOR_HANDLER 4
VECTOR_HANDLER 5
VECTOR_HANDLER 6
VECTOR_HANDLER 7
VECTOR_HANDLER 8
VECTOR_HANDLER 9
VECTOR_HANDLER 10
VECTOR_HANDLER 11
VECTOR_HANDLER 12
VECTOR_HANDLER 13
VECTOR_HANDLER 14
VECTOR_HANDLER 15

.global aarch64_exception_halt
aarch64_exception_halt:
    wfi
    b aarch64_exception_halt

.global aarch64_install_exception_vectors
.type aarch64_install_exception_vectors, %function
aarch64_install_exception_vectors:
    adrp x9, aarch64_exception_vectors
    add x9, x9, :lo12:aarch64_exception_vectors
    mrs x1, CurrentEL
    cmp x1, #4
    b.eq 9f
    cmp x1, #8
    b.eq 10f
    mov x0, #0
    ret
9:
    msr VBAR_EL1, x9
    isb
    mov x0, #1
    ret
10:
    msr VBAR_EL2, x9
    isb
    mov x0, #2
    ret
.size aarch64_install_exception_vectors, . - aarch64_install_exception_vectors

.global aarch64_exception_test_fault
.type aarch64_exception_test_fault, %function
aarch64_exception_test_fault:
    msr SPSel, #1
    isb
.global aarch64_exception_test_instruction
aarch64_exception_test_instruction:
    .inst 0x00000000
    msr SPSel, #0
    isb
    ret
.size aarch64_exception_test_fault, . - aarch64_exception_test_fault

.section .note.GNU-stack, "", %progbits
