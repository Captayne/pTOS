/*
 * rp2350_uart.c - RP2350 UART0 (PL011) console
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * UART0 on GPIO0 (TX) and GPIO1 (RX), 115200 8N1.  The RP2350's UARTs are
 * ARM PL011s, the same as QEMU virt-arm's; see virt_uart.c.
 *
 * Everything written here also goes to the USB console (rp2350_usbcon.c),
 * and input from both is accepted, so the system console works with a
 * USB-serial adapter on the UART pins, over USB, or both.
 */

#include "emutos.h"
#include "rp2350.h"
#include "rp2350_uart.h"
#include "ikbd.h"
#include "rp2350_usbcon.h"
#include "earlyfault.h"

#define BAUDRATE        115200UL
#define UART_TX_PIN     0
#define UART_RX_PIN     1
#define GPIO_FUNC_UART  2

#define UART0_DR    RP2350_REG(RP2350_UART0_BASE + 0x00)
#define UART0_FR    RP2350_REG(RP2350_UART0_BASE + 0x18)
#define UART0_IBRD  RP2350_REG(RP2350_UART0_BASE + 0x24)
#define UART0_FBRD  RP2350_REG(RP2350_UART0_BASE + 0x28)
#define UART0_LCRH  RP2350_REG(RP2350_UART0_BASE + 0x2c)
#define UART0_CR    RP2350_REG(RP2350_UART0_BASE + 0x30)
#define UART0_IMSC  RP2350_REG(RP2350_UART0_BASE + 0x38)
#define UART0_ICR   RP2350_REG(RP2350_UART0_BASE + 0x44)

#define UART_FR_RXFE    0x10UL
#define UART_FR_TXFF    0x20UL

void rp2350_uart0_init(void)
{
    ULONG baud16 = BAUDRATE * 16;
    ULONG int_div = RP2350_CLK_PERI_HZ / baud16;
    ULONG fractdiv2 = (RP2350_CLK_PERI_HZ % baud16) * 8 / BAUDRATE;
    ULONG fractdiv = fractdiv2 / 2 + fractdiv2 % 2;

    UART0_CR = 0;
    UART0_IMSC = 0;
    UART0_ICR = 0x7ff;
    UART0_IBRD = int_div;
    UART0_FBRD = fractdiv;
    UART0_LCRH = (3 << 5) | (1 << 4);  /* 8 bits, no parity, FIFOs enabled */
    UART0_CR = 0x301;                  /* UARTEN | TXE | RXE */

    rp2350_gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    rp2350_gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
}

BOOL rp2350_uart0_can_write(void)
{
    return (UART0_FR & UART_FR_TXFF) == 0;
}

void rp2350_uart0_write_byte(UBYTE b)
{
    rp2350_usbcon_putc(b);
    while (!rp2350_uart0_can_write())
        ;
    UART0_DR = b;
}

/* bios/arch/armv8m/earlyfault.c: raw output that works from reset on */
void armv8m_debug_putc(char c)
{
    rp2350_uart0_write_byte((UBYTE)c);
}

BOOL rp2350_uart0_can_read(void)
{
    return (UART0_FR & UART_FR_RXFE) == 0 || rp2350_usbcon_can_read();
}

UBYTE rp2350_uart0_read_byte(void)
{
    for (;;)
    {
        if ((UART0_FR & UART_FR_RXFE) == 0)
            return (UBYTE)UART0_DR;
        if (rp2350_usbcon_can_read())
            return rp2350_usbcon_getc();
    }
}

/* Feeds typed characters into the emulated IKBD queue, polled from the
 * 200 Hz system timer; see virt_uart0_poll_rx() for the reasoning. */
void rp2350_uart0_poll_rx(void)
{
#if CONF_SERIAL_CONSOLE
    /* USB input is pushed from the USB interrupt directly */
    while ((UART0_FR & UART_FR_RXFE) == 0)
        push_ascii_ikbdiorec((UBYTE)UART0_DR);
#endif
}
