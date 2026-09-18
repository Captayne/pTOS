/*
 * earlyfault.c - report a fault taken before the vector table is set up
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * bios/arch/armv8m/vectorsasm.S dispatches faults through the emulated 68k
 * vector table, which init_exc_vec() only fills in during biosmain().  A
 * fault before that would find a null vector and lock the core up.
 * Instead it comes here, and the registers are printed with nothing but
 * the machine's armv8m_debug_putc(), which has to work from reset on.
 */

#include "config.h"
#include "portab.h"
#include "earlyfault.h"

static void putstr(const char *s)
{
    while (*s)
        armv8m_debug_putc(*s++);
}

static void puthex(ULONG v)
{
    int i;

    for (i = 28; i >= 0; i -= 4)
        armv8m_debug_putc("0123456789abcdef"[(v >> i) & 0xf]);
}

static void putreg(const char *name, ULONG v)
{
    putstr(name);
    puthex(v);
}

/* frame: the exception_frame_t of bios/arch/arm/vectors.c */
void armv8m_early_fault(int vector, ULONG *frame, ULONG fsr, ULONG far)
{
    int i;

    putstr("\r\n*** early fault, vector ");
    puthex((ULONG)vector);
    putreg("\r\npc=", frame[15]);
    putreg(" lr=", frame[14]);
    putreg(" sp=", frame[13]);
    putreg(" xpsr=", frame[16]);
    putreg("\r\nfsr=", fsr);
    putreg(" far=", far);
    for (i = 0; i < 13; i++)
    {
        putstr(i % 4 ? " r" : "\r\nr");
        armv8m_debug_putc(i < 10 ? '0' + i : '1');
        if (i >= 10)
            armv8m_debug_putc('0' + i - 10);
        putreg("=", frame[i]);
    }
    putstr("\r\n");

    halt();
}
