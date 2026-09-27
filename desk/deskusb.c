/*
 * deskusb.c - lending the SD card to the other machine, from the menu
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * A checked item: while it is ticked, the machine at the other end of
 * the USB cable sees the whole SD card -- partition table and all -- as
 * a removable disk, and this one refuses every drive on it.  Unticked,
 * the card comes back reported as changed, so GEMDOS re-reads what it
 * had cached; that is the floppy swap mechanism, doing what it was
 * always for.  Everything goes through the _UDR cookie
 * (include/usbdrv.h).
 *
 * The menu label still says "flash" because it lives in the binary
 * resource (desk/desktop.rsc), which wants a resource editor.
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
        form_alert(1, "[1][There is no card to share.][ OK ]");
        return;
    }

    if (udr->shared())
    {
        udr->share(0);
        return;
    }

    if (form_alert(1, "[2][The card goes to the machine|"
                      "at the other end of the cable.|"
                      "C: and D: cannot be used here|"
                      "meanwhile.  Eject it there|"
                      "before unticking.][Share|Cancel]") != 1)
        return;

    /*
     * It refuses while a deploy is in flight: that is writing to a drive
     * on this very card, and handing it over underneath would pull the
     * medium out from under GEMDOS's buffers.
     */
    switch (udr->share(1))
    {
    case 0:
        break;
    case -36:                   /* EACCDN: the interlock */
        form_alert(1, "[1][Not while an upload is|running.][ OK ]");
        break;
    case -11:                   /* EREADF: it let go, but cannot read */
        form_alert(1, "[1][The card was handed over but|"
                      "cannot be read.][ OK ]");
        break;
    default:
        form_alert(1, "[1][The card could not be|handed over.][ OK ]");
        break;
    }
}

#endif /* CONF_WITH_USB_DRIVE_MENU */
