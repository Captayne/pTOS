/*
 * rp2350_int.c - RP2350 interrupts and system timer
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Every external interrupt vector of the Cortex-M vector table (startup.S)
 * points to rp2350_irq_handler(), which looks the active interrupt up in a
 * table filled by rp2350_connect_irq() -- the same model as virt_pic.c.
 * Cortex-M exception handlers are ordinary AAPCS functions, so no assembly
 * glue is needed.
 *
 * The 200 Hz system timer (the Atari's MFP timer C) is the core's SysTick,
 * clocked by the processor clock.
 *
 * All configurable exceptions (SVCall, SysTick, external interrupts) share
 * one priority, so none of them preempts another.  The SVC trap redirect
 * in bios/arch/armv8m/vectorsasm.S relies on not being preempted while it
 * builds its frames below the active stack pointer.
 */

#include "emutos.h"
#include "rp2350.h"
#include "rp2350_int.h"
#include "rp2350_uart.h"
#include "rp2350_usbcon.h"
#include "tosvars.h"
#if CONF_WITH_RP2350_LCD
#include "rp2350_lcd.h"
#include "rp2350_touch.h"
#endif
#include "vectors.h"

#define HZ          200     /* ticks per second, as the Atari timer C */
#define PRIORITY    0x80    /* for SVCall, SysTick and all interrupts */

#define NVIC_ISER(n)    RP2350_REG(ARMV8M_NVIC_ISER0 + 4 * (n))
#define NVIC_ICER(n)    RP2350_REG(ARMV8M_NVIC_ICER0 + 4 * (n))
#define NVIC_ICPR(n)    RP2350_REG(ARMV8M_NVIC_ICPR0 + 4 * (n))
#define NVIC_IPR(n)     (*(volatile UBYTE *)(ARMV8M_NVIC_IPR0 + (n)))

#define SYST_CSR        RP2350_REG(ARMV8M_SYST_CSR)
#define SYST_RVR        RP2350_REG(ARMV8M_SYST_RVR)
#define SYST_CVR        RP2350_REG(ARMV8M_SYST_CVR)
#define SYST_CSR_ENABLE     0x1UL
#define SYST_CSR_TICKINT    0x2UL
#define SYST_CSR_CLKSOURCE  0x4UL   /* processor clock */

#define SCB_SHPR2       RP2350_REG(ARMV8M_SCB_SHPR2)
#define SCB_SHPR3       RP2350_REG(ARMV8M_SCB_SHPR3)

static PFVOID irq_handlers[RP2350_NUM_IRQS];

/*
 * The vector table, in SRAM.
 *
 * It starts out in flash, where the bootrom finds it, and that is fine
 * until the flash is being written: then nothing can be read from it --
 * neither the code of a handler nor the table that points at one.  So the
 * table is copied here and VTOR is pointed at the copy, which is what
 * makes it possible to leave one interrupt running while a sector is
 * erased.  See rp2350_int_only() below, and .ramtext in rp2350_flash.c
 * and rp2350_uart1.c.
 *
 * VTOR wants the table aligned to the next power of two above its size:
 * 16 + 52 entries -> 512 bytes.
 */
static ULONG ram_vectors[16 + RP2350_NUM_IRQS] __attribute__((aligned(512)));

/* what was enabled before rp2350_int_only() took the rest away */
#define IRQ_WORDS   ((RP2350_NUM_IRQS + 31) / 32)
static ULONG irq_enabled_before[IRQ_WORDS];

void rp2350_int_init(void)
{
    int i;

    /* the table into SRAM, and VTOR onto the copy */
    {
        const ULONG *from = (const ULONG *)RP2350_REG(ARMV8M_SCB_VTOR);

        for (i = 0; i < 16 + RP2350_NUM_IRQS; i++)
            ram_vectors[i] = from[i];
        RP2350_REG(ARMV8M_SCB_VTOR) = (ULONG)ram_vectors;
        __asm__ volatile ("dsb\n\tisb" ::: "memory");
    }

    for (i = 0; i < RP2350_NUM_IRQS; i++)
    {
        irq_handlers[i] = NULL;
        NVIC_IPR(i) = PRIORITY;
    }
    for (i = 0; i < (RP2350_NUM_IRQS + 31) / 32; i++)
    {
        NVIC_ICER(i) = 0xffffffffUL;
        NVIC_ICPR(i) = 0xffffffffUL;
    }

    SCB_SHPR2 = (ULONG)PRIORITY << 24;                           /* SVCall */
    SCB_SHPR3 = ((ULONG)PRIORITY << 24) | ((ULONG)PRIORITY << 16); /* SysTick, PendSV */

    /* 200 Hz system timer.  vector_5ms is only set much later, by
     * init_system_timer(); rp2350_systick_handler() copes with that. */
    SYST_CSR = 0;
    SYST_RVR = RP2350_CLK_SYS_HZ / HZ - 1;
    SYST_CVR = 0;
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;

    /* the USB console, polled until now, gets its interrupt */
    rp2350_usbcon_attach_irq();
}

PFVOID rp2350_connect_irq(int irq, PFVOID handler)
{
    PFVOID old;

    if (irq < 0 || irq >= RP2350_NUM_IRQS)
    {
        KDEBUG(("rp2350_connect_irq: IRQ %d out of range\n", irq));
        return NULL;
    }

    old = irq_handlers[irq];
    irq_handlers[irq] = handler;
    if (handler)
        NVIC_ISER(irq / 32) = 1UL << (irq % 32);
    else
        NVIC_ICER(irq / 32) = 1UL << (irq % 32);

    return old;
}

/*
 * Leave one interrupt running and switch every other one off, for as long
 * as the flash is being written.  PRIMASK would be simpler, but it stops
 * everything: a 45 ms sector erase then costs five hundred characters at
 * 115200 baud, and the serial port holds thirty-two.
 *
 * Everything that stays alive has to live in SRAM: the entry stub, this
 * dispatcher, the handler and its data.  Anything else firing here would
 * read a flash that is not answering.  So the caller names the one
 * interrupt it has prepared for, and takes the rest away.
 *
 * Returns what to hand back to rp2350_int_restore().
 */
ULONG rp2350_int_only(int irq)
{
    ULONG systick;
    int i;

    for (i = 0; i < IRQ_WORDS; i++)
    {
        irq_enabled_before[i] = NVIC_ISER(i);
        NVIC_ICER(i) = 0xffffffffUL;
    }
    if (irq >= 0 && irq < RP2350_NUM_IRQS)
        NVIC_ISER(irq / 32) = 1UL << (irq % 32);

    systick = SYST_CSR & SYST_CSR_TICKINT;   /* its handler is in flash */
    SYST_CSR &= ~SYST_CSR_TICKINT;

    return systick;
}

/*
 * Ticks that never happened.
 *
 * The handler above lives in flash, so it has to be switched off while
 * the flash is written, and a 4 KB sector erase takes some 23 ms -- four
 * or five ticks of the 200 Hz clock.  Dropping them drops time itself:
 * hz_200 is what GEMDOS timeouts, the time of day and the AES's own tick
 * are counted in.  A program writing steadily would make the machine's
 * clock run slow, and not subtly -- measured under a stress test, three
 * seconds of ticks took twenty-nine seconds to arrive.
 *
 * So the caller says how long the flash was away and the clock is put
 * right afterwards.  Only the count: what the handler would have done
 * besides counting is a keyboard poll and a redraw, and doing those
 * five times over from inside a disk write buys nothing.
 */
void rp2350_systick_catchup(ULONG us)
{
    static ULONG owed;          /* microseconds not yet worth a whole tick */
    ULONG total = owed + us;

    /* Keeping the remainder matters more than it looks: an erase is worth
     * four ticks, but programming 512 bytes takes a few hundred
     * microseconds and there are thousands of those a second.  Thrown
     * away one at a time, they cost a second in every four. */
    hz_200 += total / (1000000UL / HZ);
    owed = total % (1000000UL / HZ);
}

void rp2350_int_restore(ULONG systick)
{
    int i;

    for (i = 0; i < IRQ_WORDS; i++)
        NVIC_ISER(i) = irq_enabled_before[i];
    SYST_CSR |= systick;
}

__attribute__((section(".ramtext")))
void rp2350_irq_handler(void)
{
    ULONG ipsr;
    int irq;

    __asm__ volatile ("mrs %0, ipsr" : "=r"(ipsr));
    irq = (int)(ipsr & 0x1ff) - 16;

    if (irq >= 0 && irq < RP2350_NUM_IRQS && irq_handlers[irq])
        irq_handlers[irq]();
    else
    {
        /* nobody wants it: keep it from firing again */
        if (irq >= 0 && irq < RP2350_NUM_IRQS)
            NVIC_ICER(irq / 32) = 1UL << (irq % 32);
        KDEBUG(("rp2350_irq_handler: unexpected IRQ %d\n", irq));
    }
}

void rp2350_systick_handler(void)
{
    rp2350_uart0_poll_rx();
    rp2350_usbcon_timer();
#if CONF_WITH_RP2350_LCD
    rp2350_lcd_tick();
#endif
#if CONF_WITH_RP2350_TOUCH
    rp2350_touch_tick();
#endif

    if (vector_5ms)
        vector_5ms();
}
