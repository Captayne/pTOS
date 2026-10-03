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

/*
 * The same table the _UA1 cookie publishes, for code inside pTOS.
 * The STiK service (rp2350_stik.c) drives the module through this
 * rather than looking its own cookie up: it is in the same image,
 * and a cookie is how a *program* finds a resource.
 */
struct ua1_api;
extern const struct ua1_api ua1_api;

#if CONF_WITH_RP2350_STIK
void rp2350_stik_add_cookie(void);   /* bios/machine/rp2350/rp2350_stik.c */
#endif

#endif /* RP2350_UART1_H */
