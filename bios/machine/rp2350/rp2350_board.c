/*
 * rp2350_board.c - RP2350 clocks, resets and pin set-up
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Called from startup.S before any other C code, with the BSS already
 * cleared.  Brings the system clock from the bootrom's ring oscillator
 * up to 150 MHz from the 12 MHz crystal, following the sequence described
 * in the RP2350 datasheet (chapter 8, "Clocks"), and prepares the console
 * UART so that everything from here on can print.
 */

#include "emutos.h"
#include "rp2350.h"
#include "rp2350_uart.h"

/* RESETS */
#define RESETS_RESET        RP2350_REG(RP2350_RESETS_BASE + 0x00)
#define RESETS_RESET_SET    RP2350_REG(RP2350_RESETS_BASE + RP2350_REG_SET + 0x00)
#define RESETS_RESET_CLR    RP2350_REG(RP2350_RESETS_BASE + RP2350_REG_CLR + 0x00)
#define RESETS_RESET_DONE   RP2350_REG(RP2350_RESETS_BASE + 0x08)

/* XOSC */
#define XOSC_CTRL           RP2350_REG(RP2350_XOSC_BASE + 0x00)
#define XOSC_STATUS         RP2350_REG(RP2350_XOSC_BASE + 0x04)
#define XOSC_STARTUP        RP2350_REG(RP2350_XOSC_BASE + 0x0c)
#define XOSC_CTRL_1_15MHZ   0xaa0UL
#define XOSC_CTRL_ENABLE    (0xfabUL << 12)
#define XOSC_STATUS_STABLE  0x80000000UL

/* PLL_SYS, PLL_USB */
#define PLL_CS(pll)         RP2350_REG((pll) + 0x00)
#define PLL_PWR_CLR(pll)    RP2350_REG((pll) + RP2350_REG_CLR + 0x04)
#define PLL_FBDIV_INT(pll)  RP2350_REG((pll) + 0x08)
#define PLL_PRIM(pll)       RP2350_REG((pll) + 0x0c)
#define PLL_CS_LOCK         0x80000000UL
#define PLL_PWR_PD          0x01UL
#define PLL_PWR_POSTDIVPD   0x08UL
#define PLL_PWR_VCOPD       0x20UL

/* CLOCKS */
#define CLK_REF_CTRL        RP2350_REG(RP2350_CLOCKS_BASE + 0x30)
#define CLK_REF_DIV         RP2350_REG(RP2350_CLOCKS_BASE + 0x34)
#define CLK_REF_SELECTED    RP2350_REG(RP2350_CLOCKS_BASE + 0x38)
#define CLK_SYS_CTRL        RP2350_REG(RP2350_CLOCKS_BASE + 0x3c)
#define CLK_SYS_DIV         RP2350_REG(RP2350_CLOCKS_BASE + 0x40)
#define CLK_SYS_SELECTED    RP2350_REG(RP2350_CLOCKS_BASE + 0x44)
#define CLK_PERI_CTRL       RP2350_REG(RP2350_CLOCKS_BASE + 0x48)
#define CLK_PERI_DIV        RP2350_REG(RP2350_CLOCKS_BASE + 0x4c)
#define CLK_USB_CTRL        RP2350_REG(RP2350_CLOCKS_BASE + 0x60)
#define CLK_USB_DIV         RP2350_REG(RP2350_CLOCKS_BASE + 0x64)
#define CLK_REF_SRC_XOSC    0x2UL
#define CLK_SYS_SRC_AUX     0x1UL
#define CLK_PERI_ENABLE     0x800UL
#define CLK_USB_ENABLE      0x800UL
#define CLK_DIV_1           0x10000UL       /* integer divider 1 */

/* TICKS: tick generator of TIMER0, 1 us from the 12 MHz clk_ref */
#define TICKS_TIMER0_CTRL   RP2350_REG(RP2350_TICKS_BASE + 0x18)
#define TICKS_TIMER0_CYCLES RP2350_REG(RP2350_TICKS_BASE + 0x1c)

/* IO_BANK0 / PADS_BANK0 */
#define GPIO_CTRL(n)        RP2350_REG(RP2350_IO_BANK0_BASE + 0x04 + 8 * (n))
#define PADS_GPIO(n)        RP2350_REG(RP2350_PADS_BANK0_BASE + 0x04 + 4 * (n))
#define PADS_ISO            0x100UL
#define PADS_OD             0x080UL
#define PADS_IE             0x040UL

static void unreset(ULONG bits)
{
    RESETS_RESET_CLR = bits;
    while ((RESETS_RESET_DONE & bits) != bits)
        ;
}

static void xosc_init(void)
{
    XOSC_CTRL = XOSC_CTRL_1_15MHZ;
    /* startup delay in units of 256 crystal cycles: about 1 ms */
    XOSC_STARTUP = ((RP2350_XOSC_HZ / 1000) + 128) / 256;
    XOSC_CTRL = XOSC_CTRL_ENABLE | XOSC_CTRL_1_15MHZ;
    while (!(XOSC_STATUS & XOSC_STATUS_STABLE))
        ;
}

/*
 * Start a PLL from the 12 MHz reference (no reference divider):
 * VCO = 12 MHz * fbdiv, output = VCO / postdiv1 / postdiv2.
 */
static void pll_init(ULONG pll, ULONG reset_bit, ULONG fbdiv, ULONG postdiv1, ULONG postdiv2)
{
    RESETS_RESET_SET = reset_bit;
    unreset(reset_bit);

    PLL_CS(pll) = 1;                        /* REFDIV */
    PLL_FBDIV_INT(pll) = fbdiv;
    PLL_PWR_CLR(pll) = PLL_PWR_PD | PLL_PWR_VCOPD;
    while (!(PLL_CS(pll) & PLL_CS_LOCK))
        ;
    PLL_PRIM(pll) = (postdiv1 << 16) | (postdiv2 << 12);
    PLL_PWR_CLR(pll) = PLL_PWR_POSTDIVPD;
}

static void clocks_init(void)
{
    /* Run clk_sys from clk_ref (glitchless) while everything is changed */
    CLK_SYS_CTRL &= ~CLK_SYS_SRC_AUX;
    while (CLK_SYS_SELECTED != 0x1)
        ;

    xosc_init();

    /* clk_ref from the crystal: 12 MHz */
    CLK_REF_DIV = CLK_DIV_1;
    CLK_REF_CTRL = CLK_REF_SRC_XOSC;
    while (CLK_REF_SELECTED != (1UL << CLK_REF_SRC_XOSC))
        ;

    /* PLL_SYS: 1500 MHz VCO / 5 / 2 = 150 MHz */
    pll_init(RP2350_PLL_SYS_BASE, RP2350_RESET_PLL_SYS, 125, 5, 2);
    /* PLL_USB: 1200 MHz VCO / 5 / 5 = 48 MHz */
    pll_init(RP2350_PLL_USB_BASE, RP2350_RESET_PLL_USB, 100, 5, 5);

    /* clk_sys from PLL_SYS: the aux source (bits 7:5) 0 is clksrc_pll_sys */
    CLK_SYS_DIV = CLK_DIV_1;
    CLK_SYS_CTRL = CLK_SYS_SRC_AUX;
    while (CLK_SYS_SELECTED != (1UL << CLK_SYS_SRC_AUX))
        ;

    /* clk_peri (UART, SPI) from clk_sys */
    CLK_PERI_CTRL = 0;
    CLK_PERI_DIV = CLK_DIV_1;
    CLK_PERI_CTRL = CLK_PERI_ENABLE;

    /* clk_usb from PLL_USB (aux source 0): 48 MHz */
    CLK_USB_CTRL = 0;
    CLK_USB_DIV = CLK_DIV_1;
    CLK_USB_CTRL = CLK_USB_ENABLE;

    /* TIMER0 counts microseconds */
    TICKS_TIMER0_CYCLES = RP2350_XOSC_HZ / 1000000UL;
    TICKS_TIMER0_CTRL = 1;
}

void rp2350_gpio_set_function(int gpio, int func)
{
    ULONG pad = PADS_GPIO(gpio);

    /* input enabled, output not disabled, then release the isolation
     * latch that holds every pad in its reset state (RP2350 specific) */
    pad = (pad & ~PADS_OD) | PADS_IE;
    PADS_GPIO(gpio) = pad;
    GPIO_CTRL(gpio) = func;
    PADS_GPIO(gpio) = pad & ~PADS_ISO;
}

void rp2350_board_init(void)
{
    clocks_init();

    unreset(RP2350_RESET_IO_BANK0 | RP2350_RESET_PADS_BANK0
          | RP2350_RESET_TIMER0 | RP2350_RESET_UART0 | RP2350_RESET_USBCTRL);

    rp2350_uart0_init();
}
