/*
 * startup.S - pTOS startup module for the RP2350
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * This object is linked first, so it sits at the start of flash
 * (0x10000000).  The bootrom finds the IMAGE_DEF block below within the
 * first 4 KB, and then starts the image through the Cortex-M vector table
 * at its very beginning: initial main stack pointer, then reset handler.
 */

#include "asmdefs.h"
#include "rp2350.h"
#include "rtx_rp2350.h"

        .globl  C_SYM(rp2350_reset)
        .globl  C_SYM(main)
        .globl  C_SYM(start_in_hyp)

        .extern C_SYM(stktop)
        .extern C_SYM(biosmain)
        .extern C_SYM(rp2350_board_init)
        .extern C_SYM(phystop)
        .extern __ebss

        .text

// ==== Cortex-M vector table ================================================
// VTOR requires alignment to the table size rounded up to a power of two:
// 16 + 52 entries -> 512 bytes.

        .balign 512
rp2350_vectors:
        .word   C_SYM(stktop)                   // initial MSP
        .word   C_SYM(rp2350_reset)             // Reset
#if CONF_WITH_RP2350_MONITOR
        .word   C_SYM(rp2350_nmi_entry)         // NMI: bring-up monitor
#else
        .word   C_SYM(armv8m_fault_handler)     // NMI
#endif
        .word   C_SYM(armv8m_fault_handler)     // HardFault
        .word   C_SYM(armv8m_fault_handler)     // MemManage
        .word   C_SYM(armv8m_fault_handler)     // BusFault
        .word   C_SYM(armv8m_fault_handler)     // UsageFault
        .word   C_SYM(armv8m_fault_handler)     // SecureFault
        .word   0, 0, 0                         // reserved
        .word   C_SYM(armv8m_svc_handler)       // SVCall
        .word   C_SYM(armv8m_fault_handler)     // DebugMonitor
        .word   0                               // reserved
        .word   C_SYM(armv8m_fault_handler)     // PendSV (unused)
        .word   systick_entry                   // SysTick
        .rept   RP2350_NUM_IRQS
        .word   irq_entry                       // external interrupts
        .endr

// ==== IMAGE_DEF block ======================================================
// A single-item block marking this as an ARM Secure executable for the
// RP2350 (see "Binary blocks" in the RP2350 datasheet, and pico-sdk's
// src/rp2_common/pico_crt0/embedded_start_block.inc.S).  Its link offset
// of 0 makes it a loop of one block.

        .balign 4
image_def_block:
        .word   0xffffded3              // PICOBIN_BLOCK_MARKER_START
        .byte   0x42                    // PICOBIN_BLOCK_ITEM_1BS_IMAGE_TYPE
        .byte   1                       // size in words
        .hword  0x1021                  // EXE | Secure | ARM | RP2350
        .byte   0xff                    // PICOBIN_BLOCK_ITEM_2BS_LAST
        .hword  1                       // size of the items above, in words
        .byte   0
        .word   0                       // link to the next block: itself
        .word   0xab123579              // PICOBIN_BLOCK_MARKER_END

// ==== Reset handler ========================================================

        .balign 4
ASM_FUNC(rp2350_reset):
ASM_FUNC(main):
        cpsid   i
        ldr     r0, =C_SYM(stktop)
        msr     msp, r0
        mov     r0, #0
        msr     psp, r0
        msr     control, r0             // privileged, main stack
        isb

#if defined(__ARM_FP) && (__ARM_FP > 0)
        // Let the coprocessor be touched at all.  CPACR is privileged, so
        // no program can do this for itself, and the very first floating
        // point instruction anywhere would fault without it.
        ldr     r0, =ARMV8M_SCB_CPACR
        ldr     r1, [r0]
        orr     r1, r1, #(0xF << 20)    // CP10 and CP11, full access
        str     r1, [r0]
        dsb
        isb
#endif

        ldr     r0, =rp2350_vectors
        ldr     r1, =ARMV8M_SCB_VTOR
        str     r0, [r1]

        // Clear ST-RAM up to the end of the BSS: the emulated 68k vectors
        // and system variables, .low_stram and .bss.  SRAM contents are
        // undefined after power-up.
        ldr     r0, =LOWMEM_BASE
        ldr     r1, =__ebss
        mov     r2, #0
1:      cmp     r0, r1
        bhs     2f
        str     r2, [r0], #4
        b       1b
2:
        // .ramtext (the flash driver) from flash into SRAM: it cannot run
        // from the flash it is about to erase.  After the clearing above,
        // which covers its range.
        ldr     r0, =__ramtext
        ldr     r1, =__eramtext
        ldr     r2, =__ramtext_load
1:      cmp     r0, r1
        bhs     2f
        ldr     r3, [r2], #4
        str     r3, [r0], #4
        b       1b
2:
        // Report fault causes separately instead of escalating everything
        // to HardFault: enable MemManage, BusFault, UsageFault, SecureFault.
        ldr     r0, =ARMV8M_SCB_SHCSR
        ldr     r1, [r0]
        orr     r1, r1, #0x000f0000
        str     r1, [r0]
        // Trap integer division by zero (CCR.DIV_0_TRP), as the 68k does.
        ldr     r0, =ARMV8M_SCB_CCR
        ldr     r1, [r0]
        orr     r1, r1, #0x10
        str     r1, [r0]

        bl      C_SYM(rp2350_board_init) // clocks, resets, console UART

        // top of ST-RAM; with the real-time extension, core 1 owns the
        // SRAM from RTX_RAM_BASE up
        ldr     r0, =C_SYM(phystop)
#if CONF_WITH_RP2350_RTX
        ldr     r1, =RTX_RAM_BASE
#else
        ldr     r1, =RP2350_SRAM_BASE + RP2350_SRAM_SIZE
#endif
        str     r1, [r0]

        ldr     r0, =C_SYM(stktop)
        mov     sp, r0
        b       C_SYM(biosmain)

#if CONF_WITH_RP2350_MONITOR
// NMI: hand the interrupted context's frame to the bring-up monitor
        .balign 4
ASM_FUNC(rp2350_nmi_entry):
        tst     lr, #4
        ite     eq
        mrseq   r0, msp
        mrsne   r0, psp
        b       C_SYM(rp2350_monitor_nmi)
#endif

// bios/kprint.c's dopanic() reads this on every ARM machine.
        .balign 4
C_SYM(start_in_hyp):
        .word   0

// Base of the OS, as in the OS header of the other machines; the AES uses
// it to tell whether someone has hooked trap #2 (aestrap_intercepted()).
        .globl  C_SYM(os_beg)
C_SYM(os_beg):
        .word   rp2350_vectors

// ==== Interrupt entry ======================================================
// Handler mode always runs on the main stack, which in Thread mode is also
// the stack of whatever runs there -- for the AES, the small private stack
// of the current process.  The system timer and interrupt handlers do a
// lot (mouse, VDI, AES timer), so they switch to a stack of their own.
// All of them share one priority and never preempt each other, so one
// stack is enough; only the hardware frame stays on the interrupted stack.

        .balign 4
        .type   systick_entry, %function
systick_entry:
        ldr     r1, =C_SYM(rp2350_systick_handler)
        b       1f

        .balign 4
        .type   irq_entry, %function
irq_entry:
        ldr     r1, =C_SYM(rp2350_irq_handler)
1:      mov     r0, sp
        ldr     r2, =irq_stack_top
        mov     sp, r2
        push    {r0, lr}
        blx     r1
        pop     {r0, lr}
        mov     sp, r0
        bx      lr

        .bss
        .balign 8
        .globl  C_SYM(irq_stack_bottom)     // for the bring-up monitor
C_SYM(irq_stack_bottom):
        .space  4096
irq_stack_top:
