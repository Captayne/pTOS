/*
 * gemasm.S - assembler part of GEM, ARMv8-M (Cortex-M33) version
 *
 * Copyright 2002-2017, The EmuTOS development team
 *           1999, Caldera Thin Clients, Inc.
 *           1987, Digital Research Inc.
 *           2026, The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Switching between AES processes is the kernel's business (every process
 * is one of its tasks, see gemdisp.c); what is left here is the way into
 * an accessory.
 */

#include "asmdefs.h"
#include "asmstruct.h"
#include "asmpd.h"

        .globl  C_SYM(gotopgm)
        .extern C_SYM(rlr)

        .text

/*
 * gotopgm(void)
 * entry point of accessory processes: runs on the accessory's private AES
 * stack (MSP) and enters its TEXT segment in user mode, basepage in r0.
 * Accessories set up their own stack; until then the process stack is the
 * top of the accessory's TPA.  The main stack stays where it is, which is
 * where interrupts and the next trap #2 will run.
 */
        .balign 4
ASM_FUNC(gotopgm):
        ldr     r0, =C_SYM(rlr)
        ldr     r0, [r0]
        ldr     r0, [r0, PD_LDADDR]     // basepage, stays in r0
        ldr     r1, [r0, #4]            // p_hitpa
        bic     r1, r1, #7
        msr     psp, r1
        ldr     r2, [r0, #P_TBASE]
        orr     r2, r2, #1
        mrs     r1, control
        orr     r1, r1, #3              // unprivileged, process stack
        msr     control, r1
        isb
        bx      r2
