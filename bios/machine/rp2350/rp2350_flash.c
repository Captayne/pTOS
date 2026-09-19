/*
 * rp2350_flash.c - erasing and programming the QSPI flash
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * The flash cannot be read while it is erased or programmed, and the
 * whole system normally executes from it (XIP).  The routine that does
 * the work therefore lives in .ramtext, which startup.S copies into SRAM,
 * and runs with interrupts masked -- including the monitor's NMI, whose
 * handler is in flash as well.  Core 1 is unaffected: the real-time
 * runtime runs from its own SRAM (see rtcore).
 *
 * The work itself is done by the bootrom, whose functions are found
 * through its lookup table, the same way the Pico SDK does it.  After
 * them, XIP is re-established with the set-up routine the bootrom leaves
 * in the boot RAM, and the QSPI pads and the CS1 (PSRAM) configuration
 * are restored around the call.
 */

#include "emutos.h"
#include "rp2350.h"
#include "rp2350_flash.h"
#include "asm.h"

#if CONF_WITH_RP2350_FLASHDISK

/* bootrom lookup: the halfword at 0x16 points to rom_table_lookup() */
#define ROM_TABLE_LOOKUP_ADDR   0x16UL
#define RT_FLAG_FUNC_ARM_SEC    0x0004UL
#define ROM_CODE(a,b)           ((ULONG)(a) | ((ULONG)(b) << 8))

#define BOOTRAM_BASE            0x400e0000UL    /* holds the XIP set-up code */
#define XIP_SETUP_WORDS         64

#define PADS_QSPI_IO(n)         RP2350_REG(0x40040000UL + 4 + 4 * (n))
#define PADS_QSPI_IOS           6

#define QMI_BASE                0x400d0000UL
#define QMI_M1_TIMING           RP2350_REG(QMI_BASE + 0x20)
#define QMI_M1_RFMT             RP2350_REG(QMI_BASE + 0x24)
#define QMI_M1_RCMD             RP2350_REG(QMI_BASE + 0x28)

#define EPPB_NMI_MASK0          RP2350_REG(0xe0080000UL)

#define BLOCK_SIZE              0x10000UL       /* 64 KB block erase ... */
#define BLOCK_ERASE_CMD         0xd8            /* ... the bootrom uses 4 KB
                                                 * sector erases below that */

typedef void *(*rom_lookup_t)(ULONG code, ULONG mask);

static void (*rom_connect_flash)(void);
static void (*rom_exit_xip)(void);
static void (*rom_erase)(ULONG addr, ULONG count, ULONG block_size, UBYTE cmd);
static void (*rom_program)(ULONG addr, const UBYTE *data, ULONG count);
static void (*rom_flush_cache)(void);
static void (*xip_setup)(void);
static ULONG xip_setup_code[XIP_SETUP_WORDS];
static BOOL flash_ready;

/*
 * The part that must not run from flash.  Everything it calls is either a
 * bootrom function or the copy of the XIP set-up code in SRAM.
 */
__attribute__((section(".ramtext"), noinline))
static void flash_op(ULONG offs, const UBYTE *data, ULONG count, BOOL erase)
{
    ULONG pads[PADS_QSPI_IOS];
    ULONG m1_timing, m1_rfmt, m1_rcmd, nmi_mask;
    int i;

    for (i = 0; i < PADS_QSPI_IOS; i++)
        pads[i] = PADS_QSPI_IO(i);
    m1_timing = QMI_M1_TIMING;
    m1_rfmt = QMI_M1_RFMT;
    m1_rcmd = QMI_M1_RCMD;

    /* the monitor's NMI would run from flash, which is about to go away */
    nmi_mask = EPPB_NMI_MASK0;
    EPPB_NMI_MASK0 = 0;

    rom_connect_flash();
    rom_exit_xip();
    if (erase)
        rom_erase(offs, count, BLOCK_SIZE, BLOCK_ERASE_CMD);
    else
        rom_program(offs, data, count);
    rom_flush_cache();          /* also releases the forced CS */
    xip_setup();

    EPPB_NMI_MASK0 = nmi_mask;
    QMI_M1_TIMING = m1_timing;
    QMI_M1_RFMT = m1_rfmt;
    QMI_M1_RCMD = m1_rcmd;
    for (i = 0; i < PADS_QSPI_IOS; i++)
        PADS_QSPI_IO(i) = pads[i];
}

static void run_flash_op(ULONG offs, const UBYTE *data, ULONG count, BOOL erase)
{
    ULONG primask;

    __asm__ volatile ("mrs %0, primask\n\tcpsid i" : "=r"(primask) : : "memory");
    flash_op(offs, data, count, erase);
    __asm__ volatile ("msr primask, %0" : : "r"(primask) : "memory");
}

BOOL rp2350_flash_init(void)
{
    /* volatile, so that gcc does not take the address for a null pointer
     * access at compile time */
    volatile ULONG lookup_addr = ROM_TABLE_LOOKUP_ADDR;
    rom_lookup_t lookup = (rom_lookup_t)(ULONG)*(volatile UWORD *)lookup_addr;
    const volatile ULONG *boot = (const volatile ULONG *)BOOTRAM_BASE;
    int i;

    if (flash_ready)
        return TRUE;
    if (!lookup)
        return FALSE;

    rom_connect_flash = lookup(ROM_CODE('I','F'), RT_FLAG_FUNC_ARM_SEC);
    rom_exit_xip = lookup(ROM_CODE('E','X'), RT_FLAG_FUNC_ARM_SEC);
    rom_erase = lookup(ROM_CODE('R','E'), RT_FLAG_FUNC_ARM_SEC);
    rom_program = lookup(ROM_CODE('R','P'), RT_FLAG_FUNC_ARM_SEC);
    rom_flush_cache = lookup(ROM_CODE('F','C'), RT_FLAG_FUNC_ARM_SEC);
    if (!rom_connect_flash || !rom_exit_xip || !rom_erase || !rom_program
        || !rom_flush_cache)
        return FALSE;

    /* the bootrom's XIP set-up code, which re-establishes the fast read
     * mode this image is running from */
    for (i = 0; i < XIP_SETUP_WORDS; i++)
        xip_setup_code[i] = boot[i];
    xip_setup = (void (*)(void))((ULONG)xip_setup_code | 1);    /* Thumb */

    flash_ready = TRUE;
    return TRUE;
}

/* erase 'count' bytes from 'offs' (both multiples of 4 KB) */
void rp2350_flash_erase(ULONG offs, ULONG count)
{
    if (flash_ready)
        run_flash_op(offs, NULL, count, TRUE);
}

/* program 'count' bytes (a multiple of 256) at 'offs' from RAM */
void rp2350_flash_program(ULONG offs, const UBYTE *data, ULONG count)
{
    if (flash_ready)
        run_flash_op(offs, data, count, FALSE);
}

#endif /* CONF_WITH_RP2350_FLASHDISK */
