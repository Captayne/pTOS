/*
 * rwa.S - GEMDOS assembler interface for ARMv8-M (Cortex-M33)
 *
 * Copyright (C) 1999 by Caldera Inc.
 * Copyright (C) 1985, 2001 by Authors (see below)
 * Copyright (C) 2017 The EmuTOS development team
 * Copyright (C) 2018-2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * The trap handlers below are entered through the trap redirect in
 * bios/arch/armv8m/vectorsasm.S: privileged Thread mode, main stack,
 * caller's r0-r11 in place, and r12 pointing at the caller's resume
 * record.  Unlike the A-profile version there are no banked stack
 * pointers to juggle -- the resume record already holds both of the
 * caller's stack pointers -- so a GEMDOS call only has to save the
 * argument registers below the record and remember where they are.
 *
 * Saved per process (P_REGSAVE in the basepage, as on m68k and A-profile
 * ARM): [0] result/function number, [1..4] r8-r11, [5] pointer to the
 * saved arguments r0-r7, which sit directly below the resume record.
 */

#include "asmdefs.h"
#include "asmpd.h"

        .globl  C_SYM(termuser)
        .globl  C_SYM(gouser)
        .globl  C_SYM(enter)
        .globl  C_SYM(bdos_trap2)

        .extern C_SYM(osif)
        .extern C_SYM(run)
        .extern C_SYM(old_trap2)
        .extern C_SYM(armv8m_resume)
        .extern C_SYM(armv8m_super)

        .text

//==== bdos_trap2 - trap 2 entry point =======================================
//
// This minimal interface connects the entry point for a system where the AES
// and VDI are already in memory.  trap #2 with r0 == 0 terminates the
// program (Pterm0, whose GEMDOS function number is 0 as well).
//

        .balign 4
ASM_FUNC(bdos_trap2):
        cmp     r0, #0
        beq     C_SYM(enter)
        push    {r0, r1}
        ldr     r0, =C_SYM(old_trap2)
        ldr     r0, [r0]
        str     r0, [sp, #4]
        pop     {r0, pc}                // on to the previous trap #2 handler

//==== _enter - Front end of TRAP #1 handler =================================

        .balign 4
ASM_FUNC(enter):
        cmp     r0, #0x20               // toggle/inquire state?
        beq     x20

        // The arguments go below the resume record, which may be below
        // the active stack pointer.  Masking interrupts is not enough (the
        // NMI of the bring-up monitor still pushes its frame there), so
        // the stack pointer is first moved below them.
        cpsid   i
        sub     r12, r12, #32
        cmp     sp, r12
        it      hi
        movhi   sp, r12
        stm     r12, {r0-r7}            // arguments, below the resume record

        ldr     r1, =C_SYM(run)
        ldr     r1, [r1]                // current basepage
        add     r1, r1, #P_REGSAVE
        str     r0, [r1]
        add     r2, r1, #4
        stm     r2, {r8-r11}
        str     r12, [r1, #20]          // where the arguments are

        mov     r0, r12                 // osif(pw)
        ldr     r1, =fstrt              // on to the file system stack
        mov     sp, r1
        cpsie   i
        bl      C_SYM(osif)

//...fall into gouser....

//==== _gouser ===============================================================

ASM_FUNC(gouser):
        ldr     r12, =C_SYM(run)
        ldr     r12, [r12]
        add     r12, r12, #P_REGSAVE
        str     r0, [r12]               // result
ggusr:
        ldr     r0, [r12]               // result
        add     r1, r12, #4
        ldm     r1, {r8-r11}
        ldr     r12, [r12, #20]         // saved arguments
        ldr     r4, [r12, #16]
        ldr     r5, [r12, #20]
        ldr     r6, [r12, #24]
        ldr     r7, [r12, #28]
        add     r12, r12, #32           // resume record
        b       C_SYM(armv8m_resume)

// termuser() - resume the process in run with its saved result, without
// overwriting it (see xterm() and proc_go() in bdos/proc.c)
        .balign 4
ASM_FUNC(termuser):
        ldr     r12, =C_SYM(run)
        ldr     r12, [r12]
        add     r12, r12, #P_REGSAVE
        b       ggusr

//==== function 0x20 - Super() ================================================
//
// The mode switch itself only edits the caller's resume record; see
// armv8m_super() in bios/arch/armv8m/super.c.

x20:
        mov     r0, r12                 // record, r1 = stack argument
        push    {r0, r1}
        bl      C_SYM(armv8m_super)
        pop     {r1, r2}
        mov     r12, r1
        b       C_SYM(armv8m_resume)

        .bss
        .balign 8
        .space  2000
fstrt:
        .space  8
