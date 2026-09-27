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
