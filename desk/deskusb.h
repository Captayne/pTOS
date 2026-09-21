/*
 * deskusb.h - "Share flash via USB" in the desktop
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef _DESKUSB_H
#define _DESKUSB_H

BOOL usbdrive_present(void);
BOOL usbdrive_shared(void);
void usbdrive_toggle(void);

#endif /* _DESKUSB_H */
