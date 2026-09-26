/*
 * rp2350_uart1.c - the second serial port, driven by its interrupt
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This software is licenced under the GNU Public License.
 * Please see LICENSE.TXT for further information.
 *
 * UART0 carries the console; UART1 is free and comes out on GPIO 4 and 5,
 * which is where a radio module or a sensor board goes.
 *
 * The port is driven by the interrupt, not by whoever asks: at 115200
 * baud the hardware holds 32 characters, not three milliseconds' worth,
 * and a program that politely gives the processor away is gone for a
 * system tick -- twenty milliseconds, and a hundred lost characters.  We
 * learned that the hard way, with holes in the middle of AT replies.
 *
 * So: the interrupt empties the port into a ring of our own and fills the
 * port from a second ring on the way out.  A program reads and writes the
 * rings through the "_UA1" cookie (include/uart1.h) and may take as long
 * as it likes between two looks.
 */

#include "emutos.h"
#include "rp2350.h"
#include "rp2350_int.h"
#include "rp2350_uart1.h"
#include "cookie.h"
#include "uart1.h"
#include "string.h"
#include "intmath.h"
#include "gemerror.h"

#if CONF_WITH_RP2350_UART1

#define UART1_BASE      0x40078000UL
#define UART1_DR        RP2350_REG(UART1_BASE + 0x00)
#define UART1_FR        RP2350_REG(UART1_BASE + 0x18)
#define UART1_IBRD      RP2350_REG(UART1_BASE + 0x24)
#define UART1_FBRD      RP2350_REG(UART1_BASE + 0x28)
#define UART1_LCRH      RP2350_REG(UART1_BASE + 0x2c)
#define UART1_CR        RP2350_REG(UART1_BASE + 0x30)
#define UART1_IFLS      RP2350_REG(UART1_BASE + 0x34)
#define UART1_IMSC      RP2350_REG(UART1_BASE + 0x38)
#define UART1_MIS       RP2350_REG(UART1_BASE + 0x40)
#define UART1_ICR       RP2350_REG(UART1_BASE + 0x44)

#define FR_RXFE         0x10UL          /* receive FIFO empty */
#define FR_TXFF         0x20UL          /* transmit FIFO full */

#define INT_RX          (1UL << 4)      /* receive FIFO past its level */
#define INT_TX          (1UL << 5)      /* room in the transmit FIFO */
#define INT_RT          (1UL << 6)      /* something waiting, FIFO not full */
#define INT_OE          (1UL << 10)     /* the port itself overran */

#define UART1_IRQ       34              /* UART0 is 33 */

#define RESETS_RESET_CLR    RP2350_REG(RP2350_RESETS_BASE + RP2350_REG_CLR + 0x00)
#define RESETS_RESET_DONE   RP2350_REG(RP2350_RESETS_BASE + 0x08)
#define RESET_UART1         (1UL << 27)

#define TX_PIN          4
#define RX_PIN          5
#define GPIO_FUNC_UART  2

/*
 * The rings.  Head and tail are written by different sides -- the
 * interrupt fills rx and empties tx -- so each index has one writer, and
 * that is all the exclusion this needs.
 */
#define RX_SIZE     2048
#define TX_SIZE     512

static UBYTE rx_ring[RX_SIZE];
static volatile UWORD rx_head, rx_tail;
static UBYTE tx_ring[TX_SIZE];
static volatile UWORD tx_head, tx_tail;

static volatile ULONG rx_lost;

/* ---- the interrupt ---- */

static void uart1_interrupt(void)
{
    ULONG mis = UART1_MIS;

    if (mis & INT_OE)
    {
        UART1_ICR = INT_OE;
        rx_lost++;                      /* the port itself could not keep up */
    }

    /* what came in */
    while (!(UART1_FR & FR_RXFE))
    {
        UWORD next = (UWORD)((rx_head + 1) % RX_SIZE);
        UBYTE c = (UBYTE)(UART1_DR & 0xff);

        if (next == rx_tail)
        {
            rx_lost++;                  /* nobody is reading: drop it */
            continue;
        }
        rx_ring[rx_head] = c;
        rx_head = next;
    }

    /* and what is on its way out */
    while (tx_head != tx_tail)
    {
        if (UART1_FR & FR_TXFF)
            break;
        UART1_DR = tx_ring[tx_tail];
        tx_tail = (UWORD)((tx_tail + 1) % TX_SIZE);
    }
    if (tx_head == tx_tail)
        UART1_IMSC &= ~INT_TX;          /* nothing left: stop asking */

    UART1_ICR = INT_RX | INT_RT;
}

/* ---- what a program calls ---- */

static LONG ua1_open(LONG baud)
{
    ULONG baud16, intdiv, frac2, frac;

    if (baud < 1200 || baud > 921600)
        return ERANGE;

    baud16 = (ULONG)baud * 16;
    intdiv = RP2350_CLK_PERI_HZ / baud16;
    frac2 = (RP2350_CLK_PERI_HZ % baud16) * 8 / (ULONG)baud;
    frac = frac2 / 2 + frac2 % 2;

    UART1_CR = 0;
    UART1_IMSC = 0;
    UART1_ICR = 0x7ff;
    UART1_IBRD = intdiv;
    UART1_FBRD = frac;
    UART1_LCRH = (3 << 5) | (1 << 4);   /* 8 bits, no parity, FIFOs on */
    UART1_IFLS = 0;                     /* wake us at an eighth full: 4 bytes */
    UART1_CR = 0x301;                   /* UARTEN | TXE | RXE */

    rx_head = rx_tail = tx_head = tx_tail = 0;
    rx_lost = 0;

    UART1_IMSC = INT_RX | INT_RT | INT_OE;
    return E_OK;
}

/*
 *  Everything that needs to be privileged, done once while the machine
 *  starts: the port out of reset, its two pins, and the interrupt.
 *
 *  This is why it is here and not in open().  A program runs unprivileged
 *  (bdos/proc.c starts it with CONTROL = 1), and the calls behind the
 *  cookie run in the program's own context: peripheral registers are open
 *  to it, but the interrupt controller sits in the system area and is
 *  not.  Enabling the interrupt from there killed the machine outright.
 */
void rp2350_uart1_init(void)
{
    RESETS_RESET_CLR = RESET_UART1;
    while (!(RESETS_RESET_DONE & RESET_UART1))
        ;

    UART1_CR = 0;
    UART1_IMSC = 0;
    UART1_ICR = 0x7ff;

    rp2350_gpio_set_function(TX_PIN, GPIO_FUNC_UART);
    rp2350_gpio_set_function(RX_PIN, GPIO_FUNC_UART);
    rp2350_connect_irq(UART1_IRQ, uart1_interrupt);
}

static LONG ua1_close(void)
{
    UART1_IMSC = 0;
    UART1_CR = 0;
    return E_OK;
}

static LONG ua1_status(void)
{
    return (LONG)((rx_head - rx_tail + RX_SIZE) % RX_SIZE);
}

static LONG ua1_read(void *buf, LONG len)
{
    UBYTE *p = buf;
    LONG got = 0;

    while (got < len && rx_head != rx_tail)
    {
        p[got++] = rx_ring[rx_tail];
        rx_tail = (UWORD)((rx_tail + 1) % RX_SIZE);
    }
    return got;
}

/*
 * Everything goes out, and a program that writes more than the ring holds
 * waits here for room -- with interrupts on, so the port keeps draining
 * while it waits.
 */
static LONG ua1_write(const void *buf, LONG len)
{
    const UBYTE *p = buf;
    LONG done = 0;

    while (done < len)
    {
        UWORD next;

        /*
         * Nothing queued and room in the port: straight in.  This is not
         * an optimisation but the thing that makes it work at all -- a
         * PL011 raises its transmit interrupt when the FIFO falls through
         * its threshold, so something has to have been in it first.  With
         * an empty FIFO and the interrupt merely enabled, nothing ever
         * happens: not a character out, and therefore nothing back.
         */
        if (tx_head == tx_tail && !(UART1_FR & FR_TXFF))
        {
            UART1_DR = p[done++];
            continue;
        }

        next = (UWORD)((tx_head + 1) % TX_SIZE);
        if (next == tx_tail)
            continue;                   /* full: the interrupt will drain it */
        tx_ring[tx_head] = p[done++];
        tx_head = next;
        UART1_IMSC |= INT_TX;
    }
    return done;
}

static LONG ua1_flush(void)
{
    rx_tail = rx_head;
    return E_OK;
}

static LONG ua1_lost(void)
{
    return (LONG)rx_lost;
}

static const struct ua1_api ua1_api = {
    UA1_VERSION,
    sizeof(struct ua1_api),
    ua1_open,
    ua1_close,
    ua1_status,
    ua1_read,
    ua1_write,
    ua1_flush,
    ua1_lost
};

void rp2350_uart1_add_cookie(void)
{
    cookie_add(UA1_COOKIE, (ULONG)&ua1_api);
}

#endif /* CONF_WITH_RP2350_UART1 */
