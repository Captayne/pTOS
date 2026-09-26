/*
 * memory.c - RP2350 memory initialization
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "config.h"

#ifndef MACHINE_RP2350
#error This file must only be compiled for the RP2350 target
#endif

/*
 * bios/build.mk lists memory.o unconditionally, and vpath resolves it to
 * this file for MACHINE_RP2350.  The on-chip SRAM needs no runtime
 * initialization: startup.S clears it and sets phystop.
 *
 * The PSRAM does need it, and that is what the rest of this file is.
 */

#include "emutos.h"
#include "rp2350.h"
#include "memory.h"
#include "kprint.h"

#if CONF_WITH_RP2350_PSRAM

/*
 * A QSPI PSRAM on the second chip select, as Alternate RAM.
 *
 * It cannot be anything else.  TOS memory is one block from zero to
 * phystop, and this chip answers at 0x11000000 while the on-chip SRAM is
 * at 0x20000000 -- not adjacent, and nothing can make them so.  But that
 * is the situation TOS has been in since the TT: ST-RAM in one place,
 * TT-RAM in another, and Mxalloc() to ask for either.  So the chip is
 * handed to the BDOS pool with xmaddalt() (altram_init(), bios/memory2.c)
 * and programs reach it through Mxalloc().
 *
 * What runs here must not run from flash.  Setting the chip up means
 * putting the QMI into direct mode, and in direct mode neither chip
 * select is memory mapped -- the flash stops answering, exactly as it
 * does while it is being written (rp2350_flash.c).  Hence .ramtext.
 *
 * Wiring: chip select on the pin the board says, function 9 (XIP_CS1).
 * The QMI can only bring CS1 out on GPIO 0, 8, 19 or 47, so there is not
 * much choice; the Waveshare RP2350B-PiZero uses 47 and the WeAct
 * RP2350B core board uses 0, which is what CONF_RP2350_PSRAM_CS_PIN is
 * for.
 *
 * The register values follow the pico-sdk's own PSRAM set-up, and the
 * field positions are from hardware/regs/qmi.h; getting them from memory
 * would have been guessing.
 */

#define PSRAM_CS_PIN        CONF_RP2350_PSRAM_CS_PIN
#define PSRAM_CS_FUNC       9               /* XIP_CS1 */

#if PSRAM_CS_PIN != 0 && PSRAM_CS_PIN != 8  && PSRAM_CS_PIN != 19 && PSRAM_CS_PIN != 47
#error CONF_RP2350_PSRAM_CS_PIN must be 0, 8, 19 or 47: the QMI brings its        second chip select out nowhere else
#endif

#define QMI_BASE            0x400d0000UL
#define QMI_DIRECT_CSR      RP2350_REG(QMI_BASE + 0x00)
#define QMI_DIRECT_TX       RP2350_REG(QMI_BASE + 0x04)
#define QMI_DIRECT_RX       RP2350_REG(QMI_BASE + 0x08)
#define QMI_M1_TIMING       RP2350_REG(QMI_BASE + 0x20)
#define QMI_M1_RFMT         RP2350_REG(QMI_BASE + 0x24)
#define QMI_M1_RCMD         RP2350_REG(QMI_BASE + 0x28)
#define QMI_M1_WFMT         RP2350_REG(QMI_BASE + 0x2c)
#define QMI_M1_WCMD         RP2350_REG(QMI_BASE + 0x30)

#define CSR_EN              (1UL << 0)
#define CSR_BUSY            (1UL << 1)
#define CSR_ASSERT_CS1N     (1UL << 3)
#define CSR_CLKDIV_LSB      22

#define TX_IWIDTH_Q         (2UL << 16)
#define TX_OE               (1UL << 19)
#define TX_NOPUSH           (1UL << 20)

/* M1_TIMING fields */
#define T_CLKDIV_LSB        0
#define T_RXDELAY_LSB       8
#define T_MIN_DESELECT_LSB  12
#define T_MAX_SELECT_LSB    17
#define T_PAGEBREAK_1024    (2UL << 28)
#define T_COOLDOWN_1        (1UL << 30)

/* M1_RFMT / M1_WFMT fields: quad throughout, an 8-bit command prefix */
#define FMT_ALL_QUAD        ((2UL << 0) | (2UL << 2) | (2UL << 4) \
                           | (2UL << 6) | (2UL << 8))
#define FMT_PREFIX_8        (1UL << 12)
#define FMT_DUMMY_24        (6UL << 16)      /* 24 bits = 6 quad clocks */

#if PSRAM_CS_PIN >= 32
#define CS_PIN_LEVEL    ((RP2350_REG(RP2350_SIO_GPIO_HI_IN) \
                          >> (PSRAM_CS_PIN - 32)) & 1)
#else
#define CS_PIN_LEVEL    ((RP2350_REG(RP2350_SIO_GPIO_IN) >> PSRAM_CS_PIN) & 1)
#endif

#define XIP_CTRL            RP2350_REG(0x400c8000UL)
#define XIP_CTRL_WRITABLE_M1 (1UL << 11)    /* or writes go nowhere at all */

/* the chip's own commands */
#define CMD_EXIT_QPI        0xf5
#define CMD_READ_ID         0x9f
#define CMD_ENTER_QPI       0x35
#define CMD_QUAD_READ       0xeb
#define CMD_QUAD_WRITE      0x38

#define PSRAM_MAX_HZ        133000000UL

ULONG rp2350_psram_size;        /* 0 when nothing answered */
ULONG rp2350_psram_id;          /* manufacturer, KGD, size byte */

/*
 * What the probe saw, so that a board where nothing answers can say why
 * (initinfo.c prints it).  It has to be collected rather than printed:
 * while the QMI is in direct mode the flash is not memory mapped, so
 * there is no kprintf() to call and no string to read.
 */
struct rp2350_psram_probe rp2350_psram_seen;

/*
 * Ask what is there, and set the QMI up for it.  Returns the size in
 * bytes, or 0.
 */
__attribute__((section(".ramtext"), noinline))
static ULONG psram_setup(void)
{
    ULONG divisor, period_fs, max_select, min_deselect, rxdelay;
    ULONG size = 0;
    UBYTE *id = rp2350_psram_seen.id;
    int i, pass;

    /* 75 MHz from 150: the fastest this chip family is specified for is
     * 133, and the divisor has to be whole */
    divisor = (RP2350_CLK_SYS_HZ + PSRAM_MAX_HZ - 1) / PSRAM_MAX_HZ;

    /* tCEM: the chip must not be selected for longer than 8 us at a
     * stretch, or it loses the contents it is refreshing.  MAX_SELECT
     * counts in units of 64 system clocks, so 8000 ns / 64 = 125 ns. */
    period_fs = 1000000000UL / (RP2350_CLK_SYS_HZ / 1000000UL);
    max_select = (125 * 1000000UL) / period_fs;
    min_deselect = (18 * 1000000UL + (period_fs - 1)) / period_fs
                 - (divisor + 1) / 2;

    /* One extra cycle of read delay above 100 MHz.  This is the knob to
     * turn if data comes back shifted by a nibble: it means the sample
     * point is wrong, not the wiring. */
    rxdelay = divisor;
    if (RP2350_CLK_SYS_HZ / divisor > 100000000UL)
        rxdelay++;

    /* direct mode, with a slow clock for the commands themselves */
    QMI_DIRECT_CSR = (30UL << CSR_CLKDIV_LSB) | CSR_EN;
    while (QMI_DIRECT_CSR & CSR_BUSY)
        ;

    /* An earlier boot may have left the chip in QPI mode, where it would
     * not understand a single-bit command at all -- so the command that
     * gets it out of QPI has to be sent as quad. */
    QMI_DIRECT_CSR |= CSR_ASSERT_CS1N;
    QMI_DIRECT_TX = TX_OE | TX_IWIDTH_Q | TX_NOPUSH | CMD_EXIT_QPI;
    while (QMI_DIRECT_CSR & CSR_BUSY)
        ;
    QMI_DIRECT_CSR &= ~CSR_ASSERT_CS1N;

    /*
     * Does the chip select actually move?  A pin's input can be read
     * whatever function is driving it, so this checks our own half of the
     * wiring -- the pin number and the QMI's second chip select -- without
     * needing a chip on the other end.  Asserted must read 0, released 1.
     */
    QMI_DIRECT_CSR |= CSR_ASSERT_CS1N;
    rp2350_psram_seen.cs_asserted = (UBYTE)CS_PIN_LEVEL;
    QMI_DIRECT_CSR &= ~CSR_ASSERT_CS1N;
    rp2350_psram_seen.cs_idle = (UBYTE)CS_PIN_LEVEL;

    /*
     * 0x9f, three address bytes, and then it says who it is: the
     * manufacturer at 4, the known-good-die byte at 5, the size at 6.
     *
     * Twice, because the two answers together say more than one does.  The
     * same wrong answer both times is a wire; a different one each time is
     * the sampling point -- although not at this clock, which the divider
     * of 30 above puts at 5 MHz, in single-bit SPI with no dummy cycles.
     * Nothing in this chip family has trouble at 5 MHz, so a probe that
     * fails here is not a timing problem.
     */
    for (pass = 0; pass < 2; pass++)
    {
        UBYTE *dst = pass ? rp2350_psram_seen.id2 : id;

        QMI_DIRECT_CSR |= CSR_ASSERT_CS1N;
        for (i = 0; i < 8; i++)
        {
            QMI_DIRECT_TX = (i == 0) ? CMD_READ_ID : 0x00;
            while (QMI_DIRECT_CSR & CSR_BUSY)
                ;
            dst[i] = (UBYTE)QMI_DIRECT_RX;
        }
        QMI_DIRECT_CSR &= ~CSR_ASSERT_CS1N;
    }

    if (id[5] == 0x5d)          /* the APS family says so here */
    {
        UBYTE code = id[6] >> 5;

        size = 1024UL * 1024UL;
        if (code == 4)
            size *= 16;
        else if (id[6] == 0x26 || code == 2 || code == 3)
            size *= 8;
        else if (code == 1)
            size *= 4;
        else
            size *= 2;
    }

    if (size == 0)
    {
        QMI_DIRECT_CSR = 0;     /* nothing there: leave everything alone */
        return 0;
    }

    /* into QPI, which is the only mode the memory-mapped commands use */
    QMI_DIRECT_CSR |= CSR_ASSERT_CS1N;
    QMI_DIRECT_TX = TX_NOPUSH | CMD_ENTER_QPI;
    while (QMI_DIRECT_CSR & CSR_BUSY)
        ;
    QMI_DIRECT_CSR &= ~CSR_ASSERT_CS1N;

    QMI_M1_TIMING = T_COOLDOWN_1 | T_PAGEBREAK_1024
                  | (max_select << T_MAX_SELECT_LSB)
                  | (min_deselect << T_MIN_DESELECT_LSB)
                  | (rxdelay << T_RXDELAY_LSB)
                  | (divisor << T_CLKDIV_LSB);
    QMI_M1_RFMT = FMT_ALL_QUAD | FMT_PREFIX_8 | FMT_DUMMY_24;
    QMI_M1_RCMD = CMD_QUAD_READ;
    QMI_M1_WFMT = FMT_ALL_QUAD | FMT_PREFIX_8;
    QMI_M1_WCMD = CMD_QUAD_WRITE;

    QMI_DIRECT_CSR = 0;                     /* memory mapped again */
    XIP_CTRL |= XIP_CTRL_WRITABLE_M1;       /* and writeable */

    /* what the chip reported, for the boot log to repeat */
    rp2350_psram_id = ((ULONG)id[4] << 16) | ((ULONG)id[5] << 8) | id[6];
    return size;
}

/*
 * Believing the chip is not the same as having read a byte back from it.
 * Two words far enough apart to be in different pages, then read them
 * again: a chip that is smaller than it claimed wraps around, and one
 * that is not really there returns whatever the bus last carried.
 */
static BOOL psram_works(ULONG size)
{
    volatile ULONG *low = (volatile ULONG *)RP2350_PSRAM_BASE;
    volatile ULONG *high = (volatile ULONG *)(RP2350_PSRAM_BASE + size - 4);

    *low = 0x50545331UL;        /* "PTS1" */
    *high = 0xa55aa55aUL;

    return *low == 0x50545331UL && *high == 0xa55aa55aUL;
}

void rp2350_psram_init(void)
{
    ULONG size;

    /* the chip select; the helper also releases the isolation latch in
     * the order the RP2350 wants it */
    rp2350_gpio_set_function(PSRAM_CS_PIN, PSRAM_CS_FUNC);

    size = psram_setup();

    KINFO(("psram: cs on GPIO %d reads %d asserted, %d released\n",
           PSRAM_CS_PIN, rp2350_psram_seen.cs_asserted,
           rp2350_psram_seen.cs_idle));
    KINFO(("psram: id %02x%02x%02x%02x%02x%02x%02x%02x"
           " then %02x%02x%02x%02x%02x%02x%02x%02x\n",
           rp2350_psram_seen.id[0], rp2350_psram_seen.id[1],
           rp2350_psram_seen.id[2], rp2350_psram_seen.id[3],
           rp2350_psram_seen.id[4], rp2350_psram_seen.id[5],
           rp2350_psram_seen.id[6], rp2350_psram_seen.id[7],
           rp2350_psram_seen.id2[0], rp2350_psram_seen.id2[1],
           rp2350_psram_seen.id2[2], rp2350_psram_seen.id2[3],
           rp2350_psram_seen.id2[4], rp2350_psram_seen.id2[5],
           rp2350_psram_seen.id2[6], rp2350_psram_seen.id2[7]));

    if (size == 0)
    {
        KINFO(("psram: no chip answered on GPIO %d\n", PSRAM_CS_PIN));
        return;
    }

    if (!psram_works(size))
    {
        KINFO(("psram: id %06lx says %ld MB, but it does not hold data\n",
               rp2350_psram_id, size / (1024UL * 1024UL)));
        return;
    }

    rp2350_psram_size = size;
    KINFO(("psram: %ld MB at %08lx (id %06lx)\n",
           size / (1024UL * 1024UL), (ULONG)RP2350_PSRAM_BASE,
           rp2350_psram_id));
}

#endif /* CONF_WITH_RP2350_PSRAM */
