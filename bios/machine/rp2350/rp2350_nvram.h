/*
 * rp2350_nvram.h - the settings the machine keeps when the power goes
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef RP2350_NVRAM_H
#define RP2350_NVRAM_H

/*
 * What the machine keeps.
 *
 * FIELDS ARE ADDED AT THE END AND NEVER MOVED
 *
 * A record carries its own length, so an image that knows more fields
 * than the record holds reads what is there and leaves the rest at its
 * default (rp2350_nvram.c, adopt()).  That only works while the fields
 * before it stay where they were: moving one, or re-using a name for
 * something else, makes every record written before today mean something
 * different.  Add at the end, and if a field falls out of use leave the
 * hole where it is.
 *
 * It starts nearly empty on purpose.  What belongs here -- screen
 * orientation, the time zone, the radio, which pins the system has
 * spoken for -- is a decision about the machine and not about this file,
 * and each of those wants its own thought.
 */
struct rp2350_settings {
    UWORD   version;        /* of the layout, not of the record */
    UWORD   reserved;       /* keeps what follows on a four-byte boundary */

    /*
     * Which GPIO carries the PSRAM's chip select: 0, 8, 19 or 47, and
     * nothing else -- the QMI brings its second chip select out nowhere
     * else.  The Waveshare RP2350-PiZero wires it to 47, the WeAct
     * RP2350B core board to 0, and that one number is the whole
     * difference between the two as far as this image is concerned.
     *
     * It is the first thing here that is about the board rather than
     * about taste, and it is read before the PSRAM is set up -- which is
     * possible because the record lives in the flash's XIP window, on the
     * *first* chip select, and that one the bootrom has already brought
     * up (memory.c, rp2350_board_init()).
     *
     * Zero is a legal pin, so "not set" cannot be told from "GPIO 0".
     * That is why the default is the value the image was built with
     * rather than zero: a machine with no record, or with one written
     * before this field existed, keeps behaving exactly as it did.
     */
    UBYTE   psram_cs;
    UBYTE   pad[3];         /* again, for what comes after */

    /*
     * What this machine is -- the name of a WIRING, not of a product.
     * The same core board with a display on different pins is a
     * different machine, and the first cable is where that begins; a
     * name taken from the shop cannot tell the two apart.
     *
     * It is written by whoever describes the board, in the profile, and
     * it is the only place the name is said.  What is compiled in
     * (CONF_BOARD_NAME) is what a machine believes that was never told.
     *
     * Not necessarily terminated: a record can carry 24 characters.
     * Read it through rp2350_nvram_board().
     */
    char    board[24];

    /*
     * The profile this was made from, summed.  The name is for people
     * and can be kept while the wiring underneath it changes; this
     * cannot.  It lets the tool say "your file of that name is not the
     * file this machine was configured from" instead of trusting a
     * label.  Zero means "not known", which is what every record
     * written before there was an exporter says.
     */
    ULONG   profile_sum;
};

#define RP2350_SETTINGS_VERSION 1

/*
 * Read the two sectors and take the newer valid one; if neither is, the
 * defaults stand.  Called once, early, before anything asks.
 */
void rp2350_nvram_init(void);

/*
 * Which of the two sectors is live (0, 1, or -1 for "neither was valid"),
 * and how many times the settings have been kept.  For the boot screen.
 */
WORD rp2350_nvram_slot(void);
ULONG rp2350_nvram_sequence(void);

/* What is in force.  Never NULL: defaults are settings too. */
const struct rp2350_settings *rp2350_nvram_get(void);

/* The board name, terminated, never NULL.  For the boot screen. */
const char *rp2350_nvram_board(void);

/*
 * Keep these instead.  One flash erase and one program, which is about
 * 23 milliseconds with no interrupts served and neither QSPI chip select
 * answering -- so this belongs at a moment the system has chosen, not in
 * the middle of somebody's work.
 */
LONG rp2350_nvram_put(const struct rp2350_settings *s);

/* What a machine that has never been told anything believes. */
void rp2350_nvram_defaults(struct rp2350_settings *s);

#endif /* RP2350_NVRAM_H */
