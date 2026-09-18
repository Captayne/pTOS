/*
 * setjmp.S - setjmp()/longjmp() for ARMv8-M (Thumb-2)
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * structure of jmp_buf:
 *   0: r4-r11, 32: sp, 36: lr   (fits the 11 words of util/arch/arm)
 *
 * Thumb cannot load or store sp and pc with ldm/stm, so they are moved
 * through r12.
 */

#include "asmdefs.h"

        .globl  C_SYM(setjmp)
        .globl  C_SYM(longjmp)

        .text

/* int setjmp(jmp_buf); */
        .balign 4
ASM_FUNC(setjmp):
        stmia   r0!, {r4-r11}
        mov     r12, sp
        str     r12, [r0], #4
        str     lr, [r0]
        mov     r0, #0
        bx      lr

/* void longjmp(jmp_buf, int); */
        .balign 4
ASM_FUNC(longjmp):
        ldmia   r0!, {r4-r11}
        ldr     r12, [r0], #4
        mov     sp, r12
        ldr     lr, [r0]
        movs    r0, r1                  // return value, but 1 instead of 0
        it      eq
        moveq   r0, #1
        bx      lr
