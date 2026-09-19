/*      GEMSTART.S - ARMv8-M (Cortex-M33) version */

/*
*       Copyright 1999, Caldera Thin Clients, Inc.
*                 2002-2017 The EmuTOS development team
*                 2026 The pTOS development team
*
*       This software is licenced under the GNU Public License.
*       Please see LICENSE.TXT for further information.
*
*                  Historical Copyright
*       -------------------------------------------------------------
*       GEM Application Environment Services              Version 3.0
*       Serial No.  XXXX-0000-654321              All Rights Reserved
*       Copyright (C) 1987                      Digital Research Inc.
*       -------------------------------------------------------------
*/

/*
 * Same flow as aes/arch/arm/gemstart.S.  Super(0L) on this port keeps the
 * caller on its process stack (see bios/arch/armv8m/super.c), so the AES
 * process 0 runs privileged on the process stack.  Thumb-2 cannot load sp
 * from memory or apply most data processing to it, hence the detours
 * through r1.
 */

#include "asmdefs.h"
#include "asmstruct.h"

        .globl  C_SYM(ui_start)
        .globl  C_SYM(accdesk_start)
        .globl  C_SYM(gemdos)
        .globl  C_SYM(dos_exec)

        .extern C_SYM(ad_stail)
        .extern C_SYM(ad_envrn)
        .extern C_SYM(D)
        .extern C_SYM(bzero_nobuiltin)
        .extern C_SYM(justretf)
        .extern C_SYM(drwaddr)
        .extern C_SYM(tikcod)
        .extern C_SYM(tikaddr)
        .extern C_SYM(init_p0_stkptr)
        .extern C_SYM(gem_main)
        .extern C_SYM(gl_changerez)
        .extern C_SYM(run_accs_and_desktop)
        .extern C_SYM(disable_interrupts)
        .extern C_SYM(rlr)
        .extern C_SYM(enable_interrupts)

        .text

//////////////////////////////////////////////////////////////////////////////
// AES start-up code
//////////////////////////////////////////////////////////////////////////////
        .balign 4
ASM_FUNC(ui_start):
        mov     r11, r0                 // our basepage

        ldr     r1, =gemusp             // temporary user stack
        mov     sp, r1

        ldr     ip, =C_SYM(ad_stail)    // command line in our basepage
        add     r2, r11, #0x80
        str     r2, [ip]

        // Mshrink(0, basepage, TEXT+DATA+BSS+basepage)
        ldr     r3, [r11, #PD_p_tlen]
        ldr     r2, [r11, #PD_p_dlen]
        add     r3, r3, r2
        ldr     r2, [r11, #PD_p_blen]
        add     r3, r3, r2
        add     r3, r3, #SIZEOF_PD
        mov     r2, r11
        movs    r1, #0
        movs    r0, #0x4a
        svc     1

        movs    r1, #0
        movs    r0, #0x20               // old_gem_ssp = Super(0)
        svc     1
        ldr     ip, =old_gem_ssp
        str     r0, [ip]

        ldr     ip, =save_ad_envrn
        ldr     r0, [r11, #PD_p_env]
        str     r0, [ip]

aes_restart:
        ldr     r1, =gemusp             // temporary stack again, for bzero()
        mov     sp, r1

        ldr     ip, =save_ad_envrn      // restore original environment
        ldr     r0, [ip]
        ldr     ip, =C_SYM(ad_envrn)
        str     r0, [ip]

        ldr     r1, =SIZEOF_THEGLO      // clear the 'global memory' zone
        ldr     r0, =C_SYM(D)
        bl      C_SYM(bzero_nobuiltin)

        ldr     r0, =C_SYM(justretf)
        ldr     ip, =C_SYM(drwaddr)
        str     r0, [ip]

        ldr     r0, =C_SYM(tikcod)
        ldr     ip, =C_SYM(tikaddr)
        str     r0, [ip]

        // process 0 stack: D + offset returned by init_p0_stkptr()
        bl      C_SYM(init_p0_stkptr)
        ldr     r1, =C_SYM(D)
        add     r1, r1, r0
        bic     r1, r1, #7
        mov     sp, r1

        bl      C_SYM(gem_main)

        ldr     ip, =C_SYM(gl_changerez) // resolution change: start over
        ldrh    r0, [ip]
        cmp     r0, #0
        bne     aes_restart

        ldr     r1, =old_gem_ssp        // Super(old_gem_ssp): user mode
        ldr     r1, [r1]
        movs    r0, #0x20
        svc     1

        movs    r0, #0                  // Pterm0()
        svc     1


//////////////////////////////////////////////////////////////////////////////
// Run accessories and desktop
//////////////////////////////////////////////////////////////////////////////
        .balign 4
ASM_FUNC(accdesk_start):
        mov     r11, r0                 // basepage
        ldr     r1, =C_SYM(D)
        ldr     r1, [r1, UDA_SPSUPER]   // use AES process 0 stack
        bic     r1, r1, #7
        mov     sp, r1

        ldr     r3, [r11, #PD_p_tlen]   // Mshrink to what we need
        ldr     r2, [r11, #PD_p_dlen]
        add     r3, r3, r2
        ldr     r2, [r11, #PD_p_blen]
        add     r3, r3, r2
        add     r3, r3, #SIZEOF_PD
        mov     r2, r11
        movs    r1, #0
        movs    r0, #0x4a
        svc     1

        movs    r1, #0
        movs    r0, #0x20               // Super(0)
        svc     1

        bl      C_SYM(run_accs_and_desktop)

        movs    r0, #0                  // Pterm0()
        svc     1


//////////////////////////////////////////////////////////////////////////////
// Execute a GEMDOS call (see gemdos.c).
//////////////////////////////////////////////////////////////////////////////
        .balign 4
ASM_FUNC(gemdos):
        push    {r4, lr}
        ldr     r4, [sp, #2*4]
        svc     1
        pop     {r4, pc}


//////////////////////////////////////////////////////////////////////////////
// dos_exec(mode, name, tail, env): Pexec() from the AES process 0.
//
// See aes/arch/arm/gemstart.S for the reentrancy problem: the child calls
// the AES through trap #2 in the context of this same AES process, and
// the AES trap switches to rlr->p_uda->u_spsuper, which must therefore
// point below everything this Pexec() leaves on our stack.  Here that is
// the svc exception frame (32-36 bytes), the resume record (16) and the
// GEMDOS arguments saved by bdos/arch/armv8m/rwa.S (32): 84 bytes at most.
// The trap handlers themselves run on the main stack.
//////////////////////////////////////////////////////////////////////////////
        .balign 4
ASM_FUNC(dos_exec):
        push    {r4-r8, lr}
        mov     r4, r0
        mov     r5, r1
        mov     r6, r2
        mov     r7, r3
        bl      C_SYM(disable_interrupts)

        ldr     r3, =C_SYM(rlr)
        ldr     r3, [r3]
        ldr     r3, [r3, PD_UDA]
        ldr     r8, [r3, UDA_SPSUPER]   // kept in r8 across Pexec()
        mov     r2, sp
        sub     r2, r2, #0x100
        bic     r2, r2, #7
        str     r2, [r3, UDA_SPSUPER]

        bl      C_SYM(enable_interrupts)

        mov     r1, r4                  // Pexec(mode, name, tail, env)
        mov     r2, r5
        mov     r3, r6
        mov     r4, r7
        movs    r0, #0x4B
        svc     1
        mov     r4, r0                  // result

        bl      C_SYM(disable_interrupts)
        ldr     r3, =C_SYM(rlr)
        ldr     r3, [r3]
        ldr     r3, [r3, PD_UDA]
        str     r8, [r3, UDA_SPSUPER]   // restore rlr->p_uda->u_spsuper
        bl      C_SYM(enable_interrupts)

        mov     r0, r4
        pop     {r4-r8, pc}

        .bss
        .balign 8

old_gem_ssp:
        .space  4

save_ad_envrn:                          // original environment, for restarts
        .space  4

        .balign 8
        .space  256                     // temporary user stack
gemusp:
        .space  8
