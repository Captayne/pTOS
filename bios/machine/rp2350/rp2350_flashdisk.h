/*
 * rp2350_flashdisk.h - a drive in the QSPI flash, with wear levelling
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef RP2350_FLASHDISK_H
#define RP2350_FLASHDISK_H

void rp2350_flashdisk_init(void);
LONG rp2350_flashdisk_ioctl(UWORD dev, UWORD ctrl, void *arg);
LONG rp2350_flashdisk_rw(WORD rw, LONG sector, WORD count, UBYTE *buf, WORD dev);

/* The USB side, which is not subject to the handover because it is the
   other party to it.  See rp2350_usbmsc.c. */
LONG rp2350_flashdisk_usb_rw(WORD rw, LONG sector, WORD count, UBYTE *buf);
LONG rp2350_flashdisk_set_shared(WORD on);
LONG rp2350_flashdisk_sectors(void);

#endif /* RP2350_FLASHDISK_H */
