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

/* the TEXT segment, for telling a code address from everything else */
extern UBYTE _text[];
extern UBYTE _etext[];

/* the machine's SRAM: the only memory safe to read from in here */
#define RAM_START   0x20000000UL
#define RAM_END     0x20082000UL

static BOOL in_ram(ULONG a)
{
    return (a >= RAM_START && a < RAM_END);
}

/*
 *  Could this stack word be a return address?
 *
 *  pTOS is built without frame pointers, so there is no chain of frames to
 *  walk: a report has the faulting pc and one lr, which is two levels at
 *  most. Everything above that is still on the stack though, mixed in among
 *  saved registers and locals, and a return address can be recognised. It
 *  is odd, because BL always sets the Thumb bit; it points into the TEXT
 *  segment; and the halfword just before it is the tail of the call that
 *  pushed it.
 *
 *  Checking for that call is what makes the result worth printing. Without
 *  it, every stale code pointer and every small number that happens to fall
 *  in range is offered as a caller, and the chain is noise. With it, what
 *  comes out is short and nearly all real.
 */
static BOOL is_retaddr(ULONG a)
{
    const UWORD *p;
    ULONG t = a & ~1UL;

    if (!(a & 1))                       /* the Thumb bit that BL sets */
        return FALSE;
    if (t < (ULONG)_text + 4 || t >= (ULONG)_etext)
        return FALSE;

    p = (const UWORD *)t;
    if ((p[-1] & 0xff80) == 0x4780)     /* BLX <reg>, a single halfword */
        return TRUE;
    if ((p[-1] & 0xd000) == 0xd000      /* BL <label>: second halfword... */
     && (p[-2] & 0xf800) == 0xf000)     /* ...and first */
        return TRUE;

    return FALSE;
}

/*
 *  The call chain, innermost first, as far up the stack as is worth looking.
 *  The Thumb bit is taken off so the addresses can be looked up directly.
 */
static void backtrace(ULONG sp)
{
    ULONG a = sp & ~3UL;
    ULONG end;
    int found = 0;

    if (!in_ram(a))
        return;

    end = a + 1024;                     /* deep enough to cross the AES */
    if (end > RAM_END)
        end = RAM_END;

    putstr("\r\ncalls");
    while (a < end && found < 16)
    {
        ULONG v = *(const ULONG *)a;

        if (is_retaddr(v))
        {
            putstr(found % 4 ? " " : "\r\n ");
            puthex(v & ~1UL);
            found++;
        }
        a += 4;
    }
    if (!found)
        putstr(" none");
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
    {
        /* Where the running program's entry point is, so that a pc inside
         * it can be turned back into a function name -- see the comment on
         * run_entry_point() in bdos/proc.c.  Declared here rather than by
         * including a BDOS header: this is the BIOS, and one prototype is
         * a smaller dependency than bdos/proc.h brings with it. */
        extern ULONG run_entry_point(void);

        putreg(" prg=", run_entry_point());
    }
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
    backtrace(frame[13]);
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
