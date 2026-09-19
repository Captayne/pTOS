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
 * M-profile has no banked stack pointers; there are two, MSP and PSP, and
 * CONTROL.SPSEL picks the one in use.  An AES process is suspended either
 * on its private supervisor stack (MSP, inside an AES trap) or, for AES
 * process 0 which runs gem_main() after Super(0L), on the process stack.
 * So a context is: r0-r11, CONTROL (kept in the spare u_regs[12]), the
 * stack pointer in use (u_spsuper) and the other one (u_spuser).
 */

#include "asmdefs.h"
#include "asmstruct.h"
#include "asmpd.h"

#define UDA_CONTROL     UDA_REGS + 12*4
#define CONTROL_SPSEL   2

        .globl  C_SYM(psetup)
        .globl  C_SYM(gotopgm)
        .globl  C_SYM(dsptch)
        .globl  C_SYM(switchto)

        .extern C_SYM(rlr)
        .extern C_SYM(indisp)
        .extern C_SYM(disp)

        .text

/*
 * psetup(PD *p, PFVOID codevalue)
 * pushes codevalue to the stack of process p, to be popped into pc at the
 * end of switchto() when p runs for the first time.  A fresh process has
 * all of its UDA cleared: CONTROL 0 (privileged, main stack).
 */
        .balign 4
ASM_FUNC(psetup):
        mrs     r3, primask
        cpsid   i
        ldr     r0, [r0, PD_UDA]
        ldr     r2, [r0, UDA_SPSUPER]
        orr     r1, r1, #1              // Thumb
        str     r1, [r2, #-4]!
        str     r2, [r0, UDA_SPSUPER]
        msr     primask, r3
        bx      lr

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

/*
 * dsptch() is a machine (& compiler) dependent routine which:
 *      1) saves any flags that will be trashed
 *      2) if (indisp) restore flags, return to dsptch caller
 *      3) otherwise
 *              save machine state,
 *              jump to disp
 */
        .balign 4
ASM_FUNC(dsptch):
        push    {r0-r3, lr}
        ldr     ip, =C_SYM(indisp)
        ldrb    r0, [ip]
        cbz     r0, savestate
        pop     {r0-r3, pc}

savestate:
        add     r0, r0, #1
        strb    r0, [ip]

        mrs     r3, primask
        cpsid   i

        ldr     r0, =C_SYM(rlr)
        ldr     r0, [r0]
        ldr     r1, [r0, PD_UDA]
        add     r2, r1, UDA_REGS + 4*4
        stmia   r2, {r4-r11}
        add     r2, r1, UDA_REGS
        pop     {r4-r7}                 // the caller's r0-r3
        stmia   r2, {r4-r7}             // (lr stays on the stack for switchto)

        mrs     r2, control
        str     r2, [r1, UDA_CONTROL]
        mov     r4, sp
        str     r4, [r1, UDA_SPSUPER]   // the stack in use
        tst     r2, #CONTROL_SPSEL
        ite     eq
        mrseq   r4, psp
        mrsne   r4, msp
        str     r4, [r1, UDA_SPUSER]    // and the other one

        bic     r2, r2, #CONTROL_SPSEL  // disp() runs on the main stack
        msr     control, r2
        isb
        ldr     r4, =gemasm_stack
        mov     sp, r4
        msr     primask, r3
        b       C_SYM(disp)

/*
 * switchto(UDA *u): resume the context saved in u
 */
        .balign 4
ASM_FUNC(switchto):
        mrs     ip, primask
        cpsid   i
        movs    r1, #0
        ldr     r2, =C_SYM(indisp)
        strb    r1, [r2]

        ldr     r1, [r0, UDA_CONTROL]
        ldr     r2, [r0, UDA_SPSUPER]
        ldr     r3, [r0, UDA_SPUSER]
        tst     r1, #CONTROL_SPSEL
        bne     1f
        mov     sp, r2                  // we are on the main stack
        msr     psp, r3
        b       2f
1:      msr     psp, r2
        msr     msp, r3
2:      msr     control, r1
        isb

        add     r0, r0, UDA_REGS
        ldm     r0, {r0-r11}
        msr     primask, ip
        pop     {pc}                    // switch to the next process now!

        .bss
        .balign 8
        .space  0x800                   // interrupts run on it too
gemasm_stack:
        .space  8
