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

    /*
     * The screen.  Width and height are the active pixels; the bytes a
     * framebuffer needs follow from them and from the format, and are
     * never kept here -- a size that is written down can disagree with
     * the resolution it belongs to, and a computed one cannot.
     *
     * refresh is in whole hertz.  It says nothing about the size and
     * everything about the bandwidth: a streamed panel reads the whole
     * framebuffer this many times a second, over the same QMI the
     * processor fetches its code through.  Zero means "as fast as the
     * path allows", which is what an SPI panel does -- it has no clock
     * of its own to programme.
     *
     * vram_where is the one thing here that is a decision rather than a
     * measurement, which is why it is kept at all.  See VRAM_* below.
     */
    UWORD   scr_w;
    UWORD   scr_h;
    UWORD   refresh;        /* Hz, 0 = as fast as the path allows */
    UBYTE   bpp;            /* bits per pixel: 16 is RGB565 */
    UBYTE   vram_where;     /* VRAM_AUTO, VRAM_STRAM, VRAM_PSRAM */
};

/*
 * Where the framebuffer lives.  Not a detail: it decides whether the
 * picture survives a flash write.  Every write takes both QSPI chip
 * selects for 23 ms, so a framebuffer in the PSRAM starves while the
 * settings are being kept and the image sits crooked afterwards -- but
 * one in the SRAM is fed by DMA from memory the QMI never touches, and
 * only the processor pauses.  The same machine is therefore able or
 * unable to keep a setting while it is running, depending on this byte.
 *
 * AUTO means the SRAM while the framebuffer fits with room left for
 * programs, and the PSRAM otherwise.  It is the right answer nearly
 * always; the other two are for saying otherwise on purpose.
 */
#define VRAM_AUTO   0
#define VRAM_STRAM  1
#define VRAM_PSRAM  2

/*
 * What the framebuffer needs, from what is kept: the active pixels,
 * plus the 2 KB every version of Atari TOS left after the screen
 * because programs write past its end (bios/screen.c).
 */
ULONG rp2350_nvram_vram_size(void);

/*
 * VRAM_STRAM or VRAM_PSRAM -- never VRAM_AUTO.  Resolves what AUTO
 * means on this machine, with this much memory.
 */
UBYTE rp2350_nvram_vram_where(void);

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
