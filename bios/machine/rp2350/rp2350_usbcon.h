/*
 * rp2350_usbcon.h - console over USB (CDC ACM) on the RP2350 USB controller
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef RP2350_USBCON_H
#define RP2350_USBCON_H

#ifndef MACHINE_RP2350
#error This file must only be compiled for the RP2350 target
#endif

void rp2350_usbcon_init(void);
void rp2350_usbcon_attach_irq(void);
void rp2350_usbcon_poll(void);

/*
 * Drop off the bus and come back, so the host forgets this device and
 * enumerates it again.  For a drive whose medium has just appeared: see
 * the comment on the definition.  Takes about a tenth of a second, in
 * which the console is gone.  Not for an interrupt handler.
 */
void rp2350_usbcon_reattach(void);
void rp2350_usbcon_add_cookie(void);
void rp2350_usbcon_timer(void);
void armv8m_halt_hook(void);
void rp2350_usbcon_putc(UBYTE c);
extern volatile BOOL rp2350_usbcon_break;
extern volatile BOOL rp2350_usbcon_shot;
BOOL rp2350_usbcon_can_read(void);
UBYTE rp2350_usbcon_getc(void);

#endif /* RP2350_USBCON_H */
