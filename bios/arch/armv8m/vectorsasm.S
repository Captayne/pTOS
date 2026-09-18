/*
 * vectorsasm.S - exception entry and trap dispatch for ARMv8-M (Cortex-M33)
 *
 * Copyright (C) 2001-2017 by the EmuTOS development team
 * Copyright (C) 2018-2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * pTOS keeps the m68k convention of a table of exception vectors in low
 * memory (LOWMEM_BASE, see config.h); on ARM, "trap #n" is "svc #n" and dispatches
 * through the vector at 0x80 + 4n.
 *
 * On the A-profile cores the SVC exception simply runs the trap handler in
 * SVC mode.  On M-profile, an exception handler runs in Handler mode, where
 * nothing of equal priority can preempt it -- a BIOS call waiting for a
 * key in bconin() (wfi) would never see the timer or UART interrupt.  So
 * the SVC exception only *redirects*: it records how to get back to the
 * caller, fabricates an exception frame whose PC is the trap handler, and
 * exception-returns into privileged Thread mode on the main stack.  The
 * trap handler then runs as ordinary, interruptible code.  When it
 * returns (through lr), it lands on armv8m_trap_tramp, whose "svc #255"
 * resumes the original caller.
 *
 * The resume record, built directly below the caller's exception frame on
 * the caller's own stack:
 *
 *      +0   caller's exception frame (r0-r3, r12, lr, pc, xpsr)
 *      +4   the caller's *other* stack pointer (PSP if it ran on MSP, and
 *           vice versa) -- the 68k USP/SSP pair
 *      +8   caller's CONTROL (bit 0: unprivileged), PRIMASK in bit 31
 *      +12  caller's EXC_RETURN (bit 2: frame is on the PSP)
 *
 * The trap handler receives the record's address in r12.  Keeping the
 * record on the caller's stack (like bdos/arch/arm/rwa.S, which keeps
 * the saved context on the user stack) lets nested processes each own
 * their saved context.  armv8m_resume (r0 = result, r12 = record) resumes
 * any such context, which is how GEMDOS starts and terminates processes.
 */

#include "asmdefs.h"

#define SCB_CFSR    0xe000ed28
#define SCB_HFSR    0xe000ed2c
#define SCB_MMFAR   0xe000ed34
#define SCB_BFAR    0xe000ed38

        .globl  C_SYM(gemtrap)        // call VDI/AES
        .globl  C_SYM(biostrap)       // call BIOS from C
        .globl  C_SYM(xbiostrap)      // call XBIOS from C
        .globl  C_SYM(xbios_unimpl)
        .globl  C_SYM(just_rts)
        .globl  C_SYM(just_rte)
        .globl  C_SYM(keyclick)
        .globl  C_SYM(default_etv_critic)

        .globl  C_SYM(armv8m_svc_handler)
        .globl  C_SYM(armv8m_fault_handler)
        .globl  C_SYM(armv8m_resume)

        .extern C_SYM(GSX_ENTRY)
        .extern C_SYM(bios_ent)
        .extern C_SYM(bios_vecs)
        .extern C_SYM(xbios_ent)
        .extern C_SYM(xbios_vecs)
        .extern C_SYM(xbios_do_unimpl)
        .extern C_SYM(kcl_hook)

        .text

// ==== SVC exception: redirect to the trap handler =========================

        .balign 4
ASM_FUNC(armv8m_svc_handler):
        mrs     r3, primask             // the caller's interrupt mask
        cpsid   i                       // we write below the active stack
        tst     lr, #4
        ite     eq
        mrseq   r0, msp
        mrsne   r0, psp                 // r0 -> caller's exception frame
        ldr     r1, [r0, #24]           // return address
        ldrb    r1, [r1, #-2]           // imm8 of the svc instruction
        cmp     r1, #255
        beq     svc_resume

        and     r1, r1, #15             // trap #0..#15
        ldr     r2, =LOWMEM_BASE + 0x80
        ldr     r2, [r2, r1, lsl #2]    // trap handler

        // resume record, below the caller's frame on the caller's stack
        sub     r12, r0, #16
        str     r0, [r12, #0]
        tst     lr, #4
        ite     eq
        mrseq   r1, psp                 // caller on MSP: other stack is PSP
        mrsne   r1, msp                 // caller on PSP: other stack is MSP
        str     r1, [r12, #4]
        mrs     r1, control
        orr     r1, r1, r3, lsl #31     // keep PRIMASK in bit 31
        str     r1, [r12, #8]
        str     lr, [r12, #12]

        // The trap handler runs on the main stack: below the record if the
        // caller was using it, else wherever the main stack currently is.
        tst     lr, #4
        ite     eq
        moveq   r3, r12
        mrsne   r3, msp
        bic     r3, r3, #7              // 8-byte aligned frame, no padding
        str     r12, [r3, #-8]!         // record address, for the trampoline
        sub     r3, r3, #32             // fabricated exception frame
        bic     r2, r2, #1
        str     r2, [r3, #24]           // pc: trap handler
        ldr     r2, =armv8m_trap_tramp
        str     r2, [r3, #20]           // lr: return trampoline
        mov     r2, #0x01000000
        str     r2, [r3, #28]           // xpsr: Thumb
        str     r12, [r3, #16]          // r12: resume record
        mov     r12, r3
        ldm     r0, {r0-r3}             // caller's r0-r3 are the arguments
        stm     r12, {r0-r3}
        msr     msp, r12

        mrs     r0, control
        bic     r0, r0, #1              // privileged
        msr     control, r0
        isb
        mvn     lr, #6                  // EXC_RETURN 0xfffffff9: Thread, MSP
        cpsie   i                       // traps run with interrupts enabled
        bx      lr

// svc #255: resume the context described by a resume record.  The record's
// address is the word just above the svc #255 exception frame.
svc_resume:
        ldr     r1, [r0, #0]            // result
        ldr     r3, [r0, #28]
        add     r2, r0, #32
        tst     r3, #0x200              // frame was padded to 8 bytes?
        it      ne
        addne   r2, r2, #4
        ldr     r2, [r2]                // resume record
        ldr     r3, [r2, #0]            // caller's frame
        str     r1, [r3, #0]            // caller's r0 = result
        ldr     r1, [r2, #4]            // caller's other stack pointer
        ldr     r0, [r2, #8]            // caller's CONTROL | PRIMASK << 31
        ldr     lr, [r2, #12]           // caller's EXC_RETURN
        tst     lr, #4
        ittee   ne
        msrne   psp, r3
        msrne   msp, r1
        msreq   msp, r3
        msreq   psp, r1
        tst     r0, #0x80000000
        bic     r0, r0, #0x80000000
        msr     control, r0
        isb
        bne     1f
        cpsie   i
1:      bx      lr

        .balign 4
        .type   armv8m_trap_tramp, %function
armv8m_trap_tramp:
        svc     #255

// armv8m_resume(result in r0, resume record in r12): resume a saved
// context.  Called in privileged Thread mode on the main stack.
        .balign 4
ASM_FUNC(armv8m_resume):
        push    {r12, lr}               // record address above the svc frame
        svc     #255

// ==== Faults ===============================================================

// All the fault exceptions (and NMI) come here.  An svc executed with
// PRIMASK set cannot be taken and escalates to HardFault, so that case is
// recognized and handled as the svc it was meant to be.  Everything else
// builds the frame bios/arch/arm/vectors.c's any_vec() expects and calls
// it through the emulated 68k vector for the fault: bus error (0x08) for
// HardFault/BusFault, address error (0x0c) for MemManage, illegal
// instruction (0x10) for UsageFault, privilege violation (0x20) for
// SecureFault.

        .balign 4
ASM_FUNC(armv8m_fault_handler):
        tst     lr, #4
        ite     eq
        mrseq   r0, msp
        mrsne   r0, psp                 // r0 -> faulting context's frame
        mrs     r1, ipsr
        ubfx    r1, r1, #0, #9          // exception number
        cmp     r1, #3                  // HardFault?
        bne     not_svc
        ldr     r2, =SCB_CFSR
        ldr     r2, [r2]
        cbnz    r2, not_svc             // a real fault escalated
        ldr     r2, [r0, #24]
        ldrh    r2, [r2, #-2]
        lsr     r2, r2, #8
        cmp     r2, #0xdf               // svc instruction just before pc?
        bne     not_svc
        ldr     r2, =SCB_HFSR
        mov     r3, #0x40000000         // clear HFSR.FORCED
        str     r3, [r2]
        b       C_SYM(armv8m_svc_handler)

not_svc:
        cpsid   i
        sub     sp, sp, #72             // exception_frame_t, 8-byte aligned
        ldm     r0, {r1-r3}
        stm     sp, {r1-r3}             // r0-r2
        ldr     r1, [r0, #12]
        str     r1, [sp, #12]           // r3
        add     r1, sp, #16
        stm     r1, {r4-r11}            // r4-r11
        ldr     r1, [r0, #16]
        str     r1, [sp, #48]           // r12
        add     r1, r0, #32
        str     r1, [sp, #52]           // sp before the exception
        ldr     r1, [r0, #20]
        str     r1, [sp, #56]           // lr
        ldr     r1, [r0, #24]
        str     r1, [sp, #60]           // pc
        ldr     r1, [r0, #28]
        str     r1, [sp, #64]           // xpsr

        mrs     r1, ipsr
        ubfx    r1, r1, #0, #9          // exception number
        mov     r0, #0x08
        cmp     r1, #4
        it      eq
        moveq   r0, #0x0c
        cmp     r1, #6
        it      eq
        moveq   r0, #0x10
        cmp     r1, #7
        it      eq
        moveq   r0, #0x20

        ldr     r2, =SCB_CFSR
        ldr     r2, [r2]                // fsr: CFSR
        cbnz    r2, 1f
        ldr     r2, =SCB_HFSR
        ldr     r2, [r2]                // ...or HFSR when CFSR is empty
1:      ldr     r3, =SCB_BFAR
        tst     r2, #0x8000             // BFARVALID
        it      eq
        ldreq   r3, =SCB_MMFAR
        ldr     r3, [r3]                // far

        mov     r1, sp
        // Report the raw registers first, with nothing but the machine's
        // raw output: if printing through the console faults again (the
        // console is not built for handler mode), at least this got out.
        // A fault during that report ends in armv8m_fault_report() too.
        push    {r0-r3}
        bl      C_SYM(armv8m_fault_report)
        pop     {r0-r3}
        ldr     r12, =LOWMEM_BASE
        ldr     r12, [r12, r0]
        cmp     r12, #0                 // vector table not set up yet?
        it      eq
        ldreq   r12, =C_SYM(armv8m_early_fault)
        blx     r12                     // any_vec(vector, frame, fsr, far)
2:      b       2b

// ==== Trap 2 - VDI =========================================================

/*
 * _gemtrap - VDI trap dispatcher
 *
 * Same behaviour as bios/arch/arm/vectorsasm.S: a VDI call (r0 == 0x73)
 * goes to the VDI dispatcher, r0 == -2 (vq_gdos) returns -2, r0 == -1
 * returns the dispatcher's address.  The AES later hooks trap #2 itself.
 */
        .balign 4
ASM_FUNC(gemtrap):
        cmp     r0, #0x73               // VDI call?
        beq     2f
        cmn     r0, #1                  // query for jsr:able address?
        bne     1f
        ldr     r0, =C_SYM(GSX_ENTRY)
1:      bx      lr
2:      b       C_SYM(GSX_ENTRY)        // returns through the trampoline

// ==== Critical error handler ==============================================

        .balign 4
ASM_FUNC(default_etv_critic):
        bx      lr                      // error code is already in r0

// ==== Trap 13 / 14 - BIOS / XBIOS =========================================

// Function number in r0, arguments in r1-r4 and, for the rare functions
// with more of them, r5-r10.  They are shifted down one register, and
// r5-r11 pushed so that they become the stacked arguments of an AAPCS
// call, exactly as in bios/arch/arm/vectorsasm.S.

        .balign 4
ASM_FUNC(biostrap):
        push    {r4, lr}
        push    {r5-r12}                // 8 words: keeps 8-byte alignment
        mov     r11, r0
        mov     r0, r1
        mov     r1, r2
        mov     r2, r3
        mov     r3, r4
        ldr     r5, =C_SYM(bios_ent)
        ldrh    r5, [r5]
        ldr     r4, =C_SYM(bios_vecs)
        b       1f

        .balign 4
ASM_FUNC(xbiostrap):
        push    {r4, lr}
        push    {r5-r12}
        mov     r11, r0
        mov     r0, r1
        mov     r1, r2
        mov     r2, r3
        mov     r3, r4
        ldr     r5, =C_SYM(xbios_ent)
        ldrh    r5, [r5]
        ldr     r4, =C_SYM(xbios_vecs)

1:      cmp     r11, r5                 // higher than highest number?
        bhi     3f
        ldr     r12, [r4, r11, lsl #2]
        blx     r12
        b       2f
3:      mov     r0, #0                  // return 0 when out of range
2:      pop     {r5-r12}
        pop     {r4, pc}

/*
 * (X)BIOS unimplemented function handler, only reached from the dispatch
 * above: the function number is in r11.
 */
        .balign 4
ASM_FUNC(xbios_unimpl):
        mov     r0, r11
        b       C_SYM(xbios_do_unimpl)

        .balign 4
ASM_FUNC(just_rts):
ASM_FUNC(just_rte):
        bx      lr

        .balign 4
ASM_FUNC(keyclick):
        ldr     r12, =C_SYM(kcl_hook)
        ldr     r12, [r12]
        bx      r12
