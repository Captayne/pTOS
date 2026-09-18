/*
 * rp2350_uart.h - RP2350 UART0 (PL011) console
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef RP2350_UART_H
#define RP2350_UART_H

#ifndef MACHINE_RP2350
#error This file must only be compiled for the RP2350 target
#endif

void rp2350_uart0_init(void);
BOOL rp2350_uart0_can_write(void);
void rp2350_uart0_write_byte(UBYTE b);
BOOL rp2350_uart0_can_read(void);
UBYTE rp2350_uart0_read_byte(void);
void rp2350_uart0_poll_rx(void);

#endif /* RP2350_UART_H */
