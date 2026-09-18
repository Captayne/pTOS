/*
 * rp2350_int.h - RP2350 interrupts and system timer
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef RP2350_INT_H
#define RP2350_INT_H

#ifndef MACHINE_RP2350
#error This file must only be compiled for the RP2350 target
#endif

void rp2350_int_init(void);
PFVOID rp2350_connect_irq(int irq, PFVOID handler);
void rp2350_irq_handler(void);
void rp2350_systick_handler(void);

#endif /* RP2350_INT_H */
