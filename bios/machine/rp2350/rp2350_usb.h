/*
 * rp2350_usb.h - the controller registers, shared by the two functions
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This software is licenced under the GNU Public License.
 * Please see LICENSE.TXT for further information.
 *
 * The device presents two functions on one port: a serial console
 * (rp2350_usbcon.c) and, when a program shares it, the flash drive
 * (rp2350_usbmsc.c).  The registers below belong to neither of them, so
 * they live here rather than being written out twice and drifting
 * apart.
 *
 * Endpoints follow a pattern the datasheet lays out in chapter 12.7:
 * endpoint n's control word is at 8*n for IN and 8*n+4 for OUT, its
 * buffer control at 0x80 + 8*n and 0x84 + 8*n, and its bit in
 * BUFF_STATUS is 1 << 2n for IN and 1 << (2n+1) for OUT.  Buffers are
 * placed by hand; 64 bytes each, starting after the ones the console
 * uses.
 */

#ifndef RP2350_USB_H
#define RP2350_USB_H

#include "rp2350.h"

#define USB_REGS            0x50110000UL
#define USB_REG(off)        RP2350_REG(USB_REGS + (off))
#define USB_REG_SET(off)    RP2350_REG(USB_REGS + RP2350_REG_SET + (off))
#define USB_REG_CLR(off)    RP2350_REG(USB_REGS + RP2350_REG_CLR + (off))

#define DPRAM               0x50100000UL
#define DPRAM_PTR(off)      ((volatile UBYTE *)(DPRAM + (off)))
#define EP_CTRL(off)        RP2350_REG(DPRAM + (off))
#define BUF_CTRL(off)       RP2350_REG(DPRAM + (off))

/* buffer control bits */
#define BUF_FULL            0x00008000UL
#define BUF_LAST            0x00004000UL
#define BUF_DATA1           0x00002000UL
#define BUF_STALL           0x00000800UL
#define BUF_AVAIL           0x00000400UL
#define BUF_LEN_MASK        0x000003ffUL

/* endpoint control bits */
#define EP_CTRL_ENABLE          0x80000000UL
#define EP_CTRL_INT_PER_BUFF    0x20000000UL
#define EP_CTRL_TYPE_BULK       (2UL << 26)
#define EP_CTRL_TYPE_INTERRUPT  (3UL << 26)

/* the drive's endpoint: bulk in and out, 64 bytes each */
#define EP3_IN_CTRL         0x18
#define EP3_OUT_CTRL        0x1c
#define EP3_IN_BUF_CTRL     0x98
#define EP3_OUT_BUF_CTRL    0x9c
#define EP3_IN_BUF          0x240
#define EP3_OUT_BUF         0x280
#define EP3_IN_BIT          0x40UL
#define EP3_OUT_BIT         0x80UL

#define BULK_SIZE           64

/* rp2350_usbcon.c owns the control endpoint and answers for both. */
void rp2350_usbcon_ep0_send(const UBYTE *data, UWORD len, UWORD wlength);
void rp2350_usbcon_buf_ctrl(ULONG off, ULONG value);

/* rp2350_usbmsc.c, called from the console's interrupt handler */
void rp2350_usbmsc_init(void);
void rp2350_usbmsc_reset(void);
void rp2350_usbmsc_out(void);
void rp2350_usbmsc_in_done(void);
BOOL rp2350_usbmsc_request(UBYTE bmRequestType, UBYTE bRequest, UWORD wValue,
                           UWORD wIndex, UWORD wLength);

/* What a program calls, through the cookie: hand the drive to the other
   machine, or take it back.  Exclusive either way. */
LONG rp2350_usbmsc_share(WORD on);
BOOL rp2350_usbmsc_shared(void);
void rp2350_usbmsc_add_cookie(void);

#endif /* RP2350_USB_H */
