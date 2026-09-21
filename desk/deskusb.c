/*
 * deskusb.c - "Share flash via USB" in the desktop's Options menu
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * A checked item: while it is ticked, the machine at the other end of
 * the USB cable sees the flash drive as a removable disk, and this one
 * refuses every access to it.  Unticked, the drive comes back reported
 * as changed, so GEMDOS re-reads what it had cached -- the floppy swap
 * mechanism.  Everything goes through the _UDR cookie (include/usbdrv.h).
 */

#include "emutos.h"
#include "obdefs.h"
#include "aesdefs.h"
#include "aesbind.h"
#include "gemdos.h"
#include "usbdrv.h"
#include "deskusb.h"

#if CONF_WITH_USB_DRIVE_MENU

#define SSYSTEM         0x154           /* GEMDOS Ssystem() */
#define S_GETCOOKIE     8

static struct udr_api *udr;

BOOL usbdrive_present(void)
{
    /* Ssystem(), not Supexec(): the latter does not exist on ARM */
    if (!udr)
    {
        ULONG value;

        if (trap1(SSYSTEM, S_GETCOOKIE, UDR_COOKIE, &value) == 0)
            udr = (struct udr_api *)value;
    }
    return udr && udr->version >= UDR_API_VERSION && udr->sectors() > 0;
}

BOOL usbdrive_shared(void)
{
    return usbdrive_present() && udr->shared();
}

void usbdrive_toggle(void)
{
    if (!usbdrive_present())
    {
        form_alert(1, "[1][There is no flash drive|to share.][ OK ]");
        return;
    }

    if (udr->shared())
    {
        udr->share(0);
        return;
    }

    if (form_alert(1, "[2][F: goes to the machine at the|"
                      "other end of the USB cable and|"
                      "cannot be used here meanwhile.|"
                      "Eject it there before|unticking.][Share|Cancel]") != 1)
        return;

    if (udr->share(1) != 0)
        form_alert(1, "[1][The drive could not be|handed over.][ OK ]");
}

#endif /* CONF_WITH_USB_DRIVE_MENU */
