/*
 * rp2350_monitor.c - bring-up monitor: a heartbeat NMI
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Without a debug probe, a system that hangs with interrupts masked is
 * invisible: the USB console is only serviced by its interrupt.  The
 * RP2350 can route any interrupt to the NMI (M33_EPPB NMI_MASK), which
 * PRIMASK does not block.  TIMER0 alarm 0 is routed there and fires every
 * 10 ms: it keeps the USB console going regardless, and once a second it
 * prints where the interrupted code was -- but only while the system is
 * stuck: inside an exception handler, or with interrupts masked, for more
 * than a second.  A healthy system never shows a line.  This is only
 * meant for bringing up the port (CONF_WITH_RP2350_MONITOR).
 */

#include "emutos.h"
#include "rp2350.h"
#include "rp2350_usbcon.h"
#include "rp2350_monitor.h"

#define TIMER0_TIMERAWL     RP2350_REG(RP2350_TIMER0_BASE + 0x28)
#define TIMER0_ALARM0       RP2350_REG(RP2350_TIMER0_BASE + 0x10)
#define TIMER0_INTR         RP2350_REG(RP2350_TIMER0_BASE + 0x3c)
#define TIMER0_INTE         RP2350_REG(RP2350_TIMER0_BASE + 0x40)
#define EPPB_NMI_MASK0      RP2350_REG(0xe0080000UL)

#define PERIOD_US           10000UL     /* 10 ms */
#define REPORT_EVERY        100         /* once a second */

static ULONG stuck_ticks;

static void mon_puts(const char *s)
{
    while (*s)
        rp2350_usbcon_putc((UBYTE)*s++);
}

static void mon_puthex(ULONG v)
{
    int i;

    for (i = 28; i >= 0; i -= 4)
        rp2350_usbcon_putc((UBYTE)"0123456789abcdef"[(v >> i) & 0xf]);
}

/* called from rp2350_nmi_entry (startup.S) with the interrupted frame */
void rp2350_monitor_nmi(ULONG *frame)
{
    TIMER0_INTR = 1;
    TIMER0_ALARM0 = TIMER0_TIMERAWL + PERIOD_US;

    {
        ULONG primask;

        __asm__ volatile ("mrs %0, primask" : "=r"(primask));
        if ((frame[7] & 0x1ff) != 0 || (primask & 1))
            stuck_ticks++;
        else
            stuck_ticks = 0;
    }

    if (stuck_ticks && stuck_ticks % REPORT_EVERY == 0)
    {
        mon_puts("\r\n[mon] pc=");
        mon_puthex(frame[6]);
        mon_puts(" lr=");
        mon_puthex(frame[5]);
        mon_puts(" xpsr=");
        mon_puthex(frame[7]);
        mon_puts("\r\n");
    }

    rp2350_usbcon_poll();
}

void rp2350_monitor_init(void)
{
    TIMER0_INTE = 0;
    TIMER0_INTR = 1;
    EPPB_NMI_MASK0 |= 1UL << RP2350_TIMER0_IRQ_0;
    TIMER0_ALARM0 = TIMER0_TIMERAWL + PERIOD_US;
    TIMER0_INTE = 1;
}
