/*
 * rp2350_uart1.h - the second serial port
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef RP2350_UART1_H
#define RP2350_UART1_H

void rp2350_uart1_init(void);       /* at boot: pins and interrupt */
void rp2350_uart1_add_cookie(void);

#endif /* RP2350_UART1_H */
