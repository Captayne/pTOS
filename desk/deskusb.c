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
#include "string.h"     /* sprintf */

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
        /*
         * Take it back, and say something only if something went wrong.
         *
         * Making this work took a long evening of not knowing which end
         * was at fault -- the card, the transport, or the other machine's
         * idea of what we are -- and the counters that finally settled it
         * earned their keep.  They are not worth four dialogs on the way
         * out of a handover that went fine, though, so they stay
         * available to any program through the cookie (include/usbdrv.h,
         * struct udr_stats) and what surfaces here is only what a person
         * has to be told: that the other machine never came for the card,
         * or that the card refused it something.  A quiet unshare means
         * it went well.
         */
        struct udr_stats st;
        char msg[160];

        memset(&st, 0, sizeof(st));
        if (udr->version >= 2)
            udr->stats(&st, sizeof(st));
        udr->share(0);

        msg[0] = '\0';
        if (udr->version < 2)
            ;                       /* it cannot tell us anything */
        else if (st.commands == 0)
            strcpy(msg, "[1][The other machine never asked|"
                        "for the card.][ OK ]");
        else if (st.reads == 0)
            sprintf(msg, "[1][The other machine asked %lu times|"
                         "but never read a sector.][ OK ]", st.commands);
        else if (st.failed != 0)
            sprintf(msg, "[1][The card refused %lu of the %lu|"
                         "sectors it was asked for.|"
                         "The last error was %ld.][ OK ]",
                    st.failed, st.reads, st.last_error);

        if (msg[0])
            form_alert(1, msg);
        return;
    }

    /*
     * The USB port is let go of and taken again so that the other machine
     * meets the drive with the card already in it -- see
     * rp2350_usbcon_reattach().  The serial console rides on the same
     * port, so it disappears for a tenth of a second and comes back; a
     * terminal holding it will say so, which is worth warning about
     * rather than leaving someone to wonder.
     */
    if (form_alert(1, "[2][The card goes to the machine|"
                      "at the other end of the cable.|"
                      "C: and D: are not usable here|"
                      "meanwhile; eject it there before|"
                      "unticking.  The console blinks.][Share|Cancel]") != 1)
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
