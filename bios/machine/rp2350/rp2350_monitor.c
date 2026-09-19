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
 * than a second.  A healthy system never shows a line.  A break sent by
 * the terminal program asks for one report on the spot.  This is only
 * meant for bringing up the port (CONF_WITH_RP2350_MONITOR).
 *
 * With the SPI display, a break also dumps the 1 bpp framebuffer, one
 * "[fb]" line of hex per scan line, for a host script (a screenshot
 * without looking at the display).
 */

#include "emutos.h"
#include "rp2350.h"
#include "rp2350_usbcon.h"
#include "rp2350_monitor.h"
#include "tosvars.h"
#if CONF_WITH_RP2350_LCD
#include "lineavars.h"
#include "rp2350_lcd.h"
#endif

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

#if CONF_WITH_RP2350_LCD
static void mon_dump_fb(void)
{
    const UBYTE *p = v_bas_ad;
    int x, y;

    for (y = 0; y < 240; y++)
    {
        mon_puts("[fb]");
        for (x = 0; x < 320 / 8; x++, p++)
        {
            rp2350_usbcon_putc((UBYTE)"0123456789abcdef"[*p >> 4]);
            rp2350_usbcon_putc((UBYTE)"0123456789abcdef"[*p & 0xf]);
        }
        mon_puts("\r\n");
    }
}
#endif

/* free bytes at the bottom of a zero-initialised stack, for stack sizing */
static ULONG stack_free(const ULONG *bottom, ULONG size)
{
    ULONG n;

    for (n = 0; n < size / 4 && bottom[n] == 0; n++)
        ;
    return n * 4;
}

extern ULONG irq_stack_bottom[];
#if CONF_WITH_AES
extern ULONG gemasm_stack_bottom[];
LONG aes_stack_free(int i);
#endif

static void mon_stacks(void)
{
    mon_puts("[stack free] irq=");
    mon_puthex(stack_free(irq_stack_bottom, 4096));
#if CONF_WITH_AES
    mon_puts(" disp=");
    mon_puthex(stack_free(gemasm_stack_bottom, 0x800));
    mon_puts(" aes0=");
    mon_puthex(aes_stack_free(0));
    mon_puts(" aes1=");
    mon_puthex(aes_stack_free(1));
#endif
    mon_puts("\r\n");
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

    if ((stuck_ticks && stuck_ticks % REPORT_EVERY == 0) || rp2350_usbcon_break)
    {
        rp2350_usbcon_break = FALSE;
        mon_puts("\r\n[mon] pc=");
        mon_puthex(frame[6]);
        mon_puts(" lr=");
        mon_puthex(frame[5]);
        mon_puts(" xpsr=");
        mon_puthex(frame[7]);
        mon_puts("\r\n");
        mon_stacks();
#if CONF_WITH_RP2350_LCD
        mon_puts("[touch] raw x=");
        mon_puthex(rp2350_lcd_touch_raw_x);
        mon_puts(" y=");
        mon_puthex(rp2350_lcd_touch_raw_y);
        mon_puts(" mouse x=");
        mon_puthex((UWORD)linea_vars.GCURX);
        mon_puts(" y=");
        mon_puthex((UWORD)linea_vars.GCURY);
        mon_puts(" noise x16 x=");
        mon_puthex(rp2350_lcd_touch_noise_x);
        mon_puts(" y=");
        mon_puthex(rp2350_lcd_touch_noise_y);
        mon_puts("\r\n[touch] taps=");
        mon_puthex(rp2350_touch_stat[0]);
        mon_puts(" dtaps=");
        mon_puthex(rp2350_touch_stat[1]);
        mon_puts(" moves=");
        mon_puthex(rp2350_touch_stat[2]);
        mon_puts(" holds=");
        mon_puthex(rp2350_touch_stat[3]);
        mon_puts(" gap=");
        mon_puthex(rp2350_touch_stat[4]);
        mon_puts("\r\n");
        if (!stuck_ticks)
            mon_dump_fb();
#endif
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
