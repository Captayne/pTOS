/*
 * cmdasm.S - EmuCON start-up for ARMv8-M (Thumb-2)
 *
 * Copyright (C) 2002-2025 The EmuTOS development team
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Same as cli/arch/arm/cmdasm.S; only the stack pointer setup differs,
 * because Thumb cannot load sp from a literal directly.
 */

#include "asmdefs.h"

#define COMASTACKSIZE   4096    /* in bytes: must be multiple of 8 */

#define PD_p_tlen       0x0c
#define PD_p_dlen       0x14
#define PD_p_blen       0x1c
#define PD_p_env        0x2c
#define SIZEOF_PD       0x100

        .globl  C_SYM(coma_start)
        .globl  C_SYM(environment)
        .extern C_SYM(cmdmain)

        .text

        .balign 4
ASM_FUNC(coma_start):
        // Our basepage address is in r0.

        // Set our temporary user stack.
        ldr     r1, =comaustack
        mov     sp, r1

        // Save the environment string pointer from the basepage, before
        // r0 gets reused below for the Mshrink() call.
        ldr     r1, [r0, #PD_p_env]
        ldr     r2, =C_SYM(environment)
        str     r1, [r2]

        // Mshrink to the needed size for TEXT+DATA+BSS plus 256 bytes.
        ldr     r3, [r0, #PD_p_tlen]
        ldr     r2, [r0, #PD_p_dlen]
        add     r3, r3, r2
        ldr     r2, [r0, #PD_p_blen]
        add     r3, r3, r2
        add     r3, r3, #SIZEOF_PD      // size of basepage itself
        mov     r2, r0
        mov     r1, #0
        mov     r0, #0x4a               // Mshrink(0, basepage, newsize)
        svc     1

        bl      C_SYM(cmdmain)          // run the command interpreter

        mov     r0, #0                  // Pterm0()
        svc     1

        .bss

C_SYM(environment): // Environment string, from the BASEPAGE
        .space  4

        // Space for the stack:
        .balign 8
        .space  COMASTACKSIZE
comaustack:
