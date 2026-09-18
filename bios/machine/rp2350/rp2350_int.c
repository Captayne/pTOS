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

void rp2350_int_init(void)
{
    int i;

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

    if (vector_5ms)
        vector_5ms();
}
