/*
 * rp2350_spi.c - SPI for the microSD slot of the Waveshare RP2350-PiZero
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Implements the SPI primitives of bios/spi.h used by the generic SD/MMC
 * driver (bios/sd.c).  The slot is wired to SPI1 (an ARM PL022):
 * SCK GPIO30, MOSI GPIO31, MISO GPIO40.  Chip select is GPIO43, driven by
 * software because it is not one of SPI1's CSn pins.
 */

#include "emutos.h"
#include "rp2350.h"
#include "spi.h"

#if CONF_WITH_RP2350_SPI

#define SD_SCK_PIN      30
#define SD_MOSI_PIN     31
#define SD_MISO_PIN     40
#define SD_CS_PIN       43      /* GPIO32-47: the "HI" SIO registers */

#define SSPCR0          RP2350_REG(RP2350_SPI1_BASE + 0x00)
#define SSPCR1          RP2350_REG(RP2350_SPI1_BASE + 0x04)
#define SSPDR           RP2350_REG(RP2350_SPI1_BASE + 0x08)
#define SSPSR           RP2350_REG(RP2350_SPI1_BASE + 0x0c)
#define SSPCPSR         RP2350_REG(RP2350_SPI1_BASE + 0x10)

#define SSPCR0_DSS_8BIT 0x07UL      /* 8 bit frames, Motorola SPI mode 0 */
#define SSPCR1_SSE      0x02UL
#define SSPSR_TNF       0x02UL
#define SSPSR_RNE       0x04UL
#define SSPSR_BSY       0x10UL

#define CS_BIT          (1UL << (SD_CS_PIN - 32))

/*
 * SPI clock = clk_peri / (CPSDVSR * (1 + SCR)), CPSDVSR even, 2..254.
 * With clk_peri at 150 MHz:
 *   identification: 150 MHz / (250 * 2) = 300 kHz (must be 100-400 kHz)
 *   MMC:            150 MHz / (2 * 8)   = 9.4 MHz (at most 20 MHz)
 *   SD:             150 MHz / (2 * 6)   = 12.5 MHz (at most 25 MHz, and
 *                   what the board has been reported to run reliably at)
 */
static void set_clock(ULONG cpsdvsr, ULONG scr)
{
    while (SSPSR & SSPSR_BSY)
        ;
    SSPCR1 = 0;
    SSPCPSR = cpsdvsr;
    SSPCR0 = (scr << 8) | SSPCR0_DSS_8BIT;
    SSPCR1 = SSPCR1_SSE;
}

void spi_clock_ident(void)
{
    set_clock(250, 1);
}

void spi_clock_mmc(void)
{
    set_clock(2, 7);
}

void spi_clock_sd(void)
{
    set_clock(2, 5);
}

void spi_cs_assert(void)
{
    RP2350_REG(RP2350_SIO_GPIO_HI_OUT_CLR) = CS_BIT;
}

void spi_cs_unassert(void)
{
    RP2350_REG(RP2350_SIO_GPIO_HI_OUT_SET) = CS_BIT;
    spi_send_byte(0xff);    /* the card releases MISO on the next clock */
}

void spi_initialise(void)
{
    /* chip select: SIO output, high */
    RP2350_REG(RP2350_SIO_GPIO_HI_OUT_SET) = CS_BIT;
    RP2350_REG(RP2350_SIO_GPIO_HI_OE_SET) = CS_BIT;
    rp2350_gpio_set_function(SD_CS_PIN, RP2350_GPIO_FUNC_SIO);

    rp2350_gpio_set_function(SD_SCK_PIN, RP2350_GPIO_FUNC_SPI);
    rp2350_gpio_set_function(SD_MOSI_PIN, RP2350_GPIO_FUNC_SPI);
    rp2350_gpio_set_function(SD_MISO_PIN, RP2350_GPIO_FUNC_SPI);
    rp2350_gpio_pull_up(SD_MISO_PIN);   /* cards float MISO when idle */

    spi_clock_ident();
}

/* full duplex: every byte sent clocks one in */
static UBYTE transfer(UBYTE out)
{
    while (!(SSPSR & SSPSR_TNF))
        ;
    SSPDR = out;
    while (!(SSPSR & SSPSR_RNE))
        ;
    return (UBYTE)SSPDR;
}

void spi_send_byte(UBYTE c)
{
    (void)transfer(c);
}

UBYTE spi_recv_byte(void)
{
    return transfer(0xff);
}

#endif /* CONF_WITH_RP2350_SPI */
