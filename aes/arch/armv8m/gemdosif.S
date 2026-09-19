/*
 * gemdosif.S - Assembler part of the GEMDOS interface, ARMv8-M version
 *
 * Copyright 2002-2017, The EmuTOS development team
 *           1999, Caldera Thin Clients, Inc.
 *           1987, Digital Research Inc.
 *           2026, The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Trap #2 arrives through the trap redirect in bios/arch/armv8m/
 * vectorsasm.S: privileged Thread mode on the main stack, the caller's
 * r0-r11 in place, r12 pointing at the caller's resume record.  The caller
 * is resumed through that record (armv8m_resume), from whatever stack we
 * are on at that point, so the AES only has to keep the record address.
 *
 * The interrupt-time hooks (mouse, buttons, timer) run in Handler mode,
 * which always uses the main stack; unlike the A-profile version they do
 * not switch to private stacks.
 */

#include "asmdefs.h"
#include "asmstruct.h"

        .globl  C_SYM(justretf)
        .globl  C_SYM(far_mcha)
        .globl  C_SYM(aes_wheel)
        .globl  C_SYM(far_bcha)
        .globl  C_SYM(drawrat)
        .globl  C_SYM(aestrap)

        .globl  C_SYM(tikaddr)
        .globl  C_SYM(tiksav)
        .globl  C_SYM(CMP_TICK)
        .globl  C_SYM(NUM_TICK)
        .globl  C_SYM(drwaddr)
        .globl  C_SYM(tikcod)

        .extern C_SYM(rlr)
        .extern C_SYM(super)
        .extern C_SYM(b_click)
        .extern C_SYM(mchange)
        .extern C_SYM(forkq)
        .extern C_SYM(wheel_change)
        .extern C_SYM(tchange)
        .extern C_SYM(b_delay)
        .extern C_SYM(savetrap2)
        .extern C_SYM(enter)
        .extern C_SYM(armv8m_resume)

        .text

// **** Trap entry: ****
        .balign 4
ASM_FUNC(aestrap):
        cmp     r0, #0                  // opcode 0 means exit: Pterm0()
        beq     C_SYM(enter)            // (GEMDOS function 0, r12 intact)

        cmp     r0, #0xC8               // c8 and c9 are handled by us
        it      ne
        cmpne   r0, #0xC9
        beq     trapaes

        push    {r0, r1}                // chain to the old trap handler
        ldr     r0, =C_SYM(savetrap2)
        ldr     r0, [r0]
        str     r0, [sp, #4]
        pop     {r0, pc}

// super() preserves r4-r11 (AAPCS), and so does every context switch in
// between, so only the resume record needs saving.
trapaes:
        cpsid   i
        ldr     r3, =C_SYM(rlr)
        ldr     r3, [r3]
        ldr     r3, [r3, PD_UDA]
        movs    r2, #1
        strh    r2, [r3, UDA_INSUPER]   // entering super()
        str     r12, [r3, UDA_OLDSPSUPER] // resume record of the caller
        ldr     r2, [r3, UDA_SPSUPER]
        mov     sp, r2                  // private AES stack of the process
        cpsie   i
        bl      C_SYM(super)            // opcode and pb already in r0/r1
        cpsid   i

        ldr     r3, =C_SYM(rlr)         // rlr may have changed meanwhile
        ldr     r3, [r3]
        ldr     r3, [r3, PD_UDA]
        movs    r2, #0
        strh    r2, [r3, UDA_INSUPER]   // left super()
        mov     r2, sp
        str     r2, [r3, UDA_SPSUPER]
        ldr     r12, [r3, UDA_OLDSPSUPER]
        b       C_SYM(armv8m_resume)    // back to the caller, result in r0


        .balign 4
ASM_FUNC(far_bcha):
        push    {r0-r4, lr}
        bl      C_SYM(b_click)
        pop     {r0-r4, pc}

        .balign 4
ASM_FUNC(far_mcha):
        push    {r1-r5, lr}
        orr     r4, r1, r0, lsl #16     // r4 = (x << 16) | y  (fdata)
        ldr     r0, =C_SYM(mchange)
        mov     r1, r4
        bl      C_SYM(forkq)            // forkq(mchange, fdata)
        mov     r0, r4                  // return the unchanged packed coords
        pop     {r1-r5, pc}

#if CONF_WITH_EXTENDED_MOUSE
/* AES mouse wheel handler called by the VDI */
        .balign 4
ASM_FUNC(aes_wheel):
        push    {r0-r4, lr}
        mov     r1, r0
        ldr     r0, =C_SYM(wheel_change)
        bl      C_SYM(forkq)            // forkq(wheel_change, r0)
        pop     {r0-r4, pc}
#endif /* CONF_WITH_EXTENDED_MOUSE */

        .balign 4
ASM_FUNC(drawrat):
        ldr     ip, =C_SYM(drwaddr)
        ldr     ip, [ip]
        bx      ip

        .balign 4
ASM_FUNC(justretf):
        bx      lr

        .balign 4
ASM_FUNC(tikcod):
        push    {r0-r4, lr}
        ldr     r4, =C_SYM(NUM_TICK)
        ldmia   r4, {r1, r2}            // r1 <- *NUM_TICK; r2 <- *CMP_TICK
        cbz     r2, 1f                  // if (*CMP_TICK != 0) {
        adds    r1, r1, #1              //      *NUM_TICK ++;
        subs    r2, r2, #1              //      *CMP_TICK --;
        stmia   r4, {r1, r2}
        cbnz    r2, 1f                  //      if (*CMP_TICK == 0)
        ldr     r0, =C_SYM(tchange)
        bl      C_SYM(forkq)            //          forkq(tchange, *NUM_TICK);
1:                                      // }
        movs    r0, #1
        bl      C_SYM(b_delay)          // b_delay(1);
        pop     {r0-r4, lr}

        ldr     ip, =C_SYM(tiksav)      // chain to the vector in tiksav
        ldr     ip, [ip]
        bx      ip

        .bss
        .balign 4

C_SYM(drwaddr):
        .space  4
C_SYM(tikaddr):
        .space  4
C_SYM(tiksav):
        .space  4
C_SYM(NUM_TICK):
        .space  4
C_SYM(CMP_TICK):
        .space  4
