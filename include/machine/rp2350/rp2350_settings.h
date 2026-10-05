/*
 * rp2350_settings.h - the record a GEMbedded machine keeps about itself
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * This is the public half of the kept settings: the layout, and the one
 * call that reaches it from outside the kernel.  It lives in include/
 * rather than next to the driver because three different kinds of code
 * need it and only one of them is the BIOS -- the BDOS, to offer
 * Ssystem(), and an ordinary program, to read or change what the
 * machine believes about its own wiring.
 *
 * The kernel-only side (reading the flash, writing it, the defaults)
 * stays in bios/machine/rp2350/rp2350_nvram.h, where nobody outside can
 * reach it.
 */

#ifndef RP2350_SETTINGS_H
#define RP2350_SETTINGS_H

#include "portab.h"

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
 * S_SETTINGS_GET / S_SETTINGS_PUT - pTOS-only, like S_CONSOLE_DIM above,
 * and given negative mode values for the same reason: they can never
 * collide with a real MiNT mode.
 *
 * What a machine was told it is -- which pin carries the PSRAM chip
 * select, what it calls itself, how big its screen is.  Until now that
 * record could only be read by the kernel and written by nobody, which
 * made every one of those facts a thing you rebuilt the image to change.
 *
 * arg1 is a struct rp2350_settings*, arg2 its sizeof() as the caller
 * knows it, and the same min() rule as S_CONSOLE_DIM applies: fields are
 * only ever added at the end, so a caller built against an older or a
 * newer pTOS still gets the part both of them agree on.  arg2 == -1
 * returns the size this kernel implements and writes nothing.
 *
 * S_SETTINGS_PUT costs 23 milliseconds with both QSPI chip selects
 * taken, and that is not an implementation detail: a machine whose
 * framebuffer lives in the PSRAM has its scanout starve for that long
 * and comes back with the picture askew.  The call refuses there rather
 * than damaging what the caller can see, and a caller that means it
 * stops the panel first.  Where the framebuffer is in the SRAM -- which
 * is every 320x240 machine -- only the processor pauses and the picture
 * never notices, so the call goes through.
 *
 * Returns bytes copied, or EINVFN / ERANGE (see the implementation).
 */
#define S_SETTINGS_GET  ((WORD)0xfffd)
#define S_SETTINGS_PUT  ((WORD)0xfffc)

/*
 * The Ssystem() entry point, called from bdos/ssystem.c for
 * S_SETTINGS_GET and S_SETTINGS_PUT.  It lives on this side of the wall
 * so that the BDOS never sees the flash, the record's two sectors or
 * anything else about how this machine keeps what it is told.
 */
LONG rp2350_settings_ssystem(WORD mode, LONG arg1, LONG arg2);

#endif /* RP2350_SETTINGS_H */
