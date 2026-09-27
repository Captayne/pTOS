/*
 * usbdrv.h - hand the SD card to the machine at the other end
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
 * The machine offers its SD card over USB as an ordinary removable disk
 * -- the whole card, partition table and all, so that everything on it
 * can be opened in a file manager.  The interface is always announced;
 * what a program decides is whether there is a medium in it.
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

/* A sketch's .ino is compiled as C++, and C++ decorates the names of
   functions: without this it would look for them in vain. */
#ifdef __cplusplus
extern "C" {
#endif

#define UDR_COOKIE      0x5F554452L     /* '_UDR' */
#define UDR_API_VERSION 6

/*
 * What the drive has been asked to do since it was last handed over.
 *
 * Here because the answer to "why does the other machine see no medium"
 * has three quite different shapes -- nothing is being asked of us, we
 * are being asked and the card refuses, or the card answers and the
 * reply does not arrive -- and telling them apart by reasoning about the
 * code has not worked.  These are the three numbers that separate them.
 */
struct udr_stats {
    unsigned long   commands;   /* command blocks that arrived at all */
    unsigned long   reads;      /* sectors the other machine asked for */
    unsigned long   failed;     /* of those, the ones the card refused */
    long            last_error; /* what it said the last time it did */
    /* since version 3 */
    unsigned long   lba_zero;   /* times sector 0 was asked for */
    unsigned long   lba_max;    /* the furthest sector asked for */
    unsigned short  signature;  /* the last two bytes of sector 0, as read */
    unsigned long   capacity;   /* sectors, as announced to the other machine */
    /* since version 4 */
    /*
     * Sector 0 at offset 0x1be: the four partition entries, sixteen bytes
     * each, exactly as the card gave them.  Here because a host that
     * refuses the medium will not say why, and the two reasons look the
     * same from this side: either these bytes are not what is on the card,
     * or they are, and they describe a disk larger than "capacity" above
     * -- which every host reads as a table that cannot belong to this
     * medium.  With the entries and the capacity side by side that is a
     * subtraction rather than a guess.
     */
    unsigned char   part[64];
    /* since version 5 */
    /*
     * The commands in the order they arrived, with the additional sense
     * code each was refused with -- 00 for one that was answered.  A
     * command we refuse never reaches the card, so it raises "commands"
     * and not "reads": without the opcodes, "commands but no reads" does
     * not distinguish a host that never asked to read from one that asked
     * in a dialect this driver rejects.
     */
    unsigned short  traced;         /* how many of the slots are filled */
    unsigned char   trace_op[20];   /* TRACE_MAX */
    unsigned char   trace_asc[20];
    /* since version 6 */
    /*
     * The transport, as opposed to the commands.  "in_packets" is the
     * decisive one: the controller reports a packet here only once the
     * host has acknowledged it, so eight per sector read means the host
     * took every byte and disliked them, and fewer means it never got
     * them.  "resets" is non-zero only if the host gave up on a transfer.
     */
    unsigned long   resets;         /* bulk-only resets the host asked for */
    unsigned long   bad_cbw;        /* command blocks that were not one */
    unsigned long   csw;            /* status wrappers handed over */
    unsigned long   in_packets;     /* IN packets the host acknowledged */
    unsigned long   dpram_bad;      /* bytes that did not survive the copy */
};

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

    /*
     * Since version 2: what has been asked of it, and how that went.
     * Counted from the last hand-over, so the numbers describe one
     * session and not the lifetime of the machine.
     *
     * `size' is sizeof(*s) as the caller knows it: the struct has grown
     * once already and will again, and a caller built against an older
     * header must not be written past the end of what it allocated.
     */
    void (*stats)(struct udr_stats *s, unsigned short size);
};


#ifdef __cplusplus
}
#endif

#endif /* USBDRV_H */
