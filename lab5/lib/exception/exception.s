.section ".data.exception"

_exception_ret_addr: .asciz "Exception return address "
_exception_class: .asciz "Exception class (EC) "
_exception_iss: .asciz "Instruction specific syndrom (ISS) "

_exception_data_abort: .asciz "Page fault @"

.section ".text.exception"

.align 11 // aligned to 0x800 (2^11)
.global _exception_table
_exception_table:
    b _exception_handler // branch to a handler function.
    .align 7 // entry size is 0x80 (2^7), .align will pad 0
    b _exception_handler
    .align 7
    b _exception_handler
    .align 7
    b _exception_handler
    .align 7

    b _exception_handler
    .align 7
    b _irq_handler
    .align 7
    b _exception_handler
    .align 7
    b _exception_handler
    .align 7

    b _exception_handler
    .align 7
    b _irq_handler
    .align 7
    b _exception_handler
    .align 7
    b _exception_handler
    .align 7

    b _exception_handler
    .align 7
    b _exception_handler
    .align 7
    b _exception_handler
    .align 7
    b _exception_handler
    .align 7

hang:
    wfe
    b hang

.macro _kernel_entry
sub sp, sp, #256 // size of all registers x0-x30 (31 * 16)
stp x0, x1, [sp, #16 * 0]
stp x2, x3, [sp, #16 * 1]
stp x4, x5, [sp, #16 * 2]
stp x6, x7, [sp, #16 * 3]
stp x8, x9, [sp, #16 * 4]
stp x10, x11, [sp, #16 * 5]
stp x12, x13, [sp, #16 * 6]
stp x14, x15, [sp, #16 * 7]
stp x16, x17, [sp, #16 * 8]
stp x18, x19, [sp, #16 * 9]
stp x20, x21, [sp, #16 * 10]
stp x22, x23, [sp, #16 * 11]
stp x24, x25, [sp, #16 * 12]
stp x26, x27, [sp, #16 * 13]
stp x28, x29, [sp, #16 * 14]
str x30, [sp, #16 * 15]

bl switchUserToKernel
.endm

.macro _kernel_exit
// exit of kernel routine
bl checkRescheduleFlag

bl switchKernelToUser

ldp x0, x1, [sp, #16 * 0]
ldp x2, x3, [sp, #16 * 1]
ldp x4, x5, [sp, #16 * 2]
ldp x6, x7, [sp, #16 * 3]
ldp x8, x9, [sp, #16 * 4]
ldp x10, x11, [sp, #16 * 5]
ldp x12, x13, [sp, #16 * 6]
ldp x14, x15, [sp, #16 * 7]
ldp x16, x17, [sp, #16 * 8]
ldp x18, x19, [sp, #16 * 9]
ldp x20, x21, [sp, #16 * 10]
ldp x22, x23, [sp, #16 * 11]
ldp x24, x25, [sp, #16 * 12]
ldp x26, x27, [sp, #16 * 13]
ldp x28, x29, [sp, #16 * 14]
ldr x30, [sp, #16 * 15]
add sp, sp, #256
.endm

.macro _interrupt_entry
// set interrupt context (stack) to position 4MB
bl getCurrentTask
mov x9, sp
str x9, [x0, 16 * 6]
mov x9, 0x3E000000
mov x10, 0xffff000000000000
orr x9, x10, x9
mov sp, x9
.endm

.macro _interrupt_exit
// reset back to kernel context (stack)
bl getCurrentTask
ldr x9, [x0, 16 * 6]
mov sp,  x9
.endm

.global _exception_handler
_exception_handler:
    _kernel_entry

    mrs x0, ESR_EL1
    // logical shift right
    // EC: [31:26]
    lsr x0, x0, #26
    cmp x0, #21 // SVC in 64-bit state
    b.eq el0_svc
    cmp x0, #36 // data abort in EL0
    b.eq el0_data_abort

el0_svc:
    mov x0, sp
    bl handleSVC
    b leave

el0_data_abort:
    ldr x0, =_exception_data_abort
    bl sendStringUART
    mrs x0, FAR_EL1
    bl sendHexUART
    mov x0, #10
    bl sendUART
    // FIXME: should pass addressable status for exit()
    bl doExit

    // ldr x0, =_exception_ret_addr
    // bl sendStringUART
    // mrs x0, ELR_EL1
    // bl sendHexUART
    // mov x0, #10
    // bl sendUART

    // ldr x0, =_exception_class
    // bl sendStringUART
    // mrs x0, ESR_EL1
    // // logical shift right
    // // EC: [31:26]
    // lsr x0, x0, #26
    // bl sendHexUART
    // mov x0, #10
    // bl sendUART

    // ldr x0, =_exception_iss
    // bl sendStringUART
    // mrs x0, ESR_EL1
    // // ISS: [24:0], 0x1ffffff (2**25 - 1)
    // and x0, x0, #0x1ffffff
    // bl sendHexUART
    // mov x0, #10
    // bl sendUART

    // mrs x0, ESR_EL1
    // and x0, x0, #0x1ffffff
    // cmp x0, #1  // 1 for exc, 2 for irq
    // b.eq leave

    // bl enable_irq
    // bl _enable_core_timer

leave:
    _kernel_exit

    eret

.global _child_return_from_fork
_child_return_from_fork:
    _kernel_exit
    eret

.global _irq_handler
_irq_handler:
    // entry of kernel routine
    _kernel_entry

    _interrupt_entry

    bl irq_handler

    _interrupt_exit

    _kernel_exit

    eret
