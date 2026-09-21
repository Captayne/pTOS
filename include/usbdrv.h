/*
 * usbdrv.h - hand the flash drive to the machine at the other end
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
 * The machine offers its flash drive over USB as an ordinary removable
 * disk, so that files can be dragged onto it from a file manager.  The
 * interface is always announced; what a program decides is whether
 * there is a medium in it.
 *
 * A program finds this through the cookie "_UDR":
 *
 *     long value;
 *     struct udr_api *d = 0;
 *     if (Ssystem(S_GETCOOKIE, UDR_COOKIE, (long)&value) == 0)
 *         d = (struct udr_api *)value;
 *
 * IT IS EXCLUSIVE, AND THAT IS THE POINT
 *
 * Two file systems with their own caches on one medium corrupt it --
 * reliably, not eventually.  So while the other machine has the drive,
 * this one does not: every access to it is refused, and when it comes
 * back the medium counts as changed, which is what makes GEMDOS throw
 * away the directory and FAT sectors it was holding.  That is the
 * mechanism TOS has always had for a floppy being swapped.
 *
 * A program that shares the drive therefore owes the user two things:
 * to say so plainly while it lasts, and to take it back before it ends.
 */

#ifndef USBDRV_H
#define USBDRV_H

#define UDR_COOKIE      0x5F554452L     /* '_UDR' */
#define UDR_API_VERSION 1

struct udr_api {
    unsigned short  version;        /* UDR_API_VERSION */
    unsigned short  size;           /* sizeof(struct udr_api) */

    /*
     * Hand the drive over (1) or take it back (0).  Returns 0, or a
     * negative TOS error: EUNDEV when there is no flash drive.
     *
     * Giving it away does not wait for the other side to finish
     * anything -- there is nothing to wait for until it starts.  Taking
     * it back is immediate too, so a program should ask the user to
     * eject it on the other machine first, exactly as with any stick.
     */
    long (*share)(short on);

    /* Non-zero while the other machine has it. */
    short (*shared)(void);

    /*
     * How many sectors of 512 bytes the drive holds, or 0 if there is
     * none.  Useful for saying how big it is before handing it over.
     */
    long (*sectors)(void);
};

#endif /* USBDRV_H */
