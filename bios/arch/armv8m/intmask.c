/*
 * intmask.c - saving and restoring the interrupt mask (ARMv8-M)
 *
 * Copyright 2002-2017, The EmuTOS development team
 * Copyright 2026, The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Same interface as bios/arch/arm/intmask.c, with PRIMASK standing in for
 * the CPSR I/F bits.  There is a single save slot, so these do not nest.
 */

#include "config.h"
#include "portab.h"
#include "asm.h"

static ULONG save_primask;

/* disable interrupts */
ULONG disable_interrupts(void)
{
    ULONG primask;

    __asm__ volatile ("mrs %0, primask\n\tcpsid i" : "=r"(primask) : : "memory");
    save_primask = primask;
    return primask;
}

/* restore interrupt mask as it was before disable_interrupts() */
void enable_interrupts(void)
{
    __asm__ volatile ("msr primask, %0" : : "r"(save_primask) : "memory");
}
