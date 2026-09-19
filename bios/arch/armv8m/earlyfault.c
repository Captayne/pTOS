/*
 * earlyfault.c - raw fault reports that do not depend on the console
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * bios/arch/armv8m/vectorsasm.S dispatches faults through the emulated 68k
 * vector table to any_vec() and dopanic(), which print through the
 * console -- in handler mode, where a second fault can only lock the core
 * up.  So every fault is first reported here with nothing but the
 * machine's armv8m_debug_putc(), which works from reset on:
 *
 *  - armv8m_fault_report() runs before the regular handling.  A fault
 *    while that is still going on is reported and stops the machine.
 *  - armv8m_early_fault() takes the place of the regular handling when
 *    the vector table is not set up yet (before init_exc_vec()).
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
static void dump(const char *what, int vector, ULONG *frame, ULONG fsr, ULONG far)
{
    int i;

    putstr(what);
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

    /* the stack from the exception frame up: shows what was being
     * returned to, where the registers alone do not */
    {
        const ULONG *p = (const ULONG *)(frame[13] - 32);

        if ((ULONG)p >= 0x20000000UL && (ULONG)p < 0x20082000UL - 32 * 4)
        {
            for (i = 0; i < 32; i++)
            {
                if (i % 8 == 0)
                {
                    putstr("\r\n");
                    puthex((ULONG)(p + i));
                    putstr(":");
                }
                putreg(" ", p[i]);
            }
        }
    }
    putstr("\r\n");
}

void armv8m_early_fault(int vector, ULONG *frame, ULONG fsr, ULONG far)
{
    dump("\r\n*** early fault, vector ", vector, frame, fsr, far);
    halt();
}

void armv8m_fault_report(int vector, ULONG *frame, ULONG fsr, ULONG far)
{
    static int nesting;

    if (nesting++)
    {
        dump("\r\n*** fault while handling a fault, vector ", vector, frame, fsr, far);
        halt();
    }
    dump("\r\n*** fault, vector ", vector, frame, fsr, far);
}
