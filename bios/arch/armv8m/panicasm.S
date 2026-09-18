/*
 * panicasm.S - panic(), halt and reset for ARMv8-M (Cortex-M33)
 *
 * Copyright (C) 2001-2017 by the EmuTOS development team
 * Copyright (C) 2018-2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "asmdefs.h"

#define SCB_AIRCR   0xe000ed0c

        .text

        .globl  C_SYM(panic)
        .globl  C_SYM(halt)
        .globl  C_SYM(kill_program)
        .globl  C_SYM(warm_reset)
        .globl  C_SYM(cold_reset)

/*
 * void panic(const char *fmt, ...);
 * save the registers and the top of the stack in the crash area, then let
 * dopanic() print the message and halt.  The exception number proc_enum
 * is zero, which tells dopanic() that fmt and its arguments follow.
 */
        .balign 4
ASM_FUNC(panic):
        cpsid   i
        push    {r12}
        ldr     r12, =C_SYM(proc_dregs)
        stmia   r12!, {r0-r11}
        pop     {r11}                   // caller's r12
        str     r11, [r12], #4
        mov     r11, sp
        str     r11, [r12], #4
        str     lr, [r12], #4
        ldr     r12, =C_SYM(proc_usp)
        mrs     r11, psp
        str     r11, [r12]

        ldr     r12, =C_SYM(proc_enum)
        mov     r0, #0
        str     r0, [r12]

        ldmia   sp, {r0-r7}             // 8 longs from the stack
        ldr     r12, =C_SYM(proc_stk)
        stmia   r12, {r0-r7}

        ldr     r12, =C_SYM(proc_lives)
        ldr     r0, =0x12345678
        str     r0, [r12]

        // restore the arguments, then on to dopanic(fmt, ...)
        ldr     r12, =C_SYM(proc_dregs)
        ldm     r12, {r0-r11}
        b       C_SYM(dopanic)

/* void halt(void) */
        .balign 4
ASM_FUNC(halt):
1:      wfi
        b       1b

/* void kill_program(void)
 * try to terminate via Pterm(); if Pterm() returns here (!), reset
 */
        .balign 4
ASM_FUNC(kill_program):
        mov     r1, #-1                 // error code for program killed by OS
        mov     r0, #0x4c               // Pterm
        svc     1
        // drop thru into warm reset

/* void warm_reset(void), void cold_reset(void)
 * Restart the system: a real system reset, through the bootrom, which
 * boots pTOS again from flash.
 */
ASM_FUNC(cold_reset):
ASM_FUNC(warm_reset):
        dsb
        ldr     r0, =SCB_AIRCR
        ldr     r1, =0x05fa0004         // VECTKEY | SYSRESETREQ
        str     r1, [r0]
        dsb
1:      b       1b
