/*
 * wifi.h - the network the machine is told to join
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This header is available under the MIT licence (unlike the rest of
 * pTOS, which is GPL), so that programs of any licence can use it:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions: The above copyright notice and this
 * permission notice shall be included in all copies or substantial
 * portions of the Software.  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT
 * WARRANTY OF ANY KIND.
 *
 * The machine has no radio of its own: a module on a serial line has it,
 * and a program talks to that module.  What the machine does keep is the
 * name of the network and its key, entered once under "Wifi settings..."
 * in the desktop's Options menu and saved with "Save desktop".  Every
 * program asks here instead of asking the user again.
 *
 * A program finds this through the cookie "_WIF":
 *
 *     long value;
 *     struct wif_api *w = 0;
 *     if (Ssystem(S_GETCOOKIE, WIF_COOKIE, (long)&value) == 0)
 *         w = (struct wif_api *)value;
 *
 * THE KEY IS NOT A SECRET HERE
 *
 * It is kept as it was typed, and EMUDESK.INF holds it in plain text --
 * a machine whose flash drive can be handed to the next computer over
 * USB cannot pretend otherwise.  Treat it as you would a note on the
 * desk: fine at home, not a place for a password you use elsewhere.
 */

#ifndef WIFI_H
#define WIFI_H

#define WIF_COOKIE      0x5F574946L     /* '_WIF' */
#define WIF_VERSION     2

#define WIF_SSID_LEN    32              /* as 802.11 has it */
#define WIF_KEY_LEN     63              /* WPA2 passphrase */

#ifdef __cplusplus
extern "C" {
#endif

struct wif_api {
    unsigned short  version;        /* WIF_VERSION */
    unsigned short  size;           /* sizeof(struct wif_api) */

    /* The network and its key, "" when nothing has been entered.  The
       strings belong to the system and stay where they are. */
    const char *(*ssid)(void);
    const char *(*key)(void);

    /*
     * Remember a network.  A program that finds nothing here can ask the
     * user itself and leave the answer for the next one; it is kept
     * until the machine is switched off, and "Save desktop" writes it to
     * EMUDESK.INF.  0 on success.
     */
    short (*set)(const char *ssid, const char *key);

    /*
     * Whole hours from UTC, as entered under Connecty: 1 for Central
     * European Time, 2 while summer time is in force.  The machine keeps
     * no rule for when that is -- it has no calendar of politics -- so
     * whoever set it decides.  (Version 2 and later.)
     */
    short (*utc_offset)(void);
    short (*set_utc_offset)(short hours);
};

#ifdef __cplusplus
}
#endif

#endif /* WIFI_H */
