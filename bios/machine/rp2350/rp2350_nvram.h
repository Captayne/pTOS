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
 * The layout itself, and VRAM_*, are public: a program may read and
 * change what the machine was told it is (include/machine/rp2350).
 * What stays here is the half nothing outside the kernel may touch.
 */
#include "rp2350_settings.h"



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
 * TRUE when this start ignored what was kept, because the rescue pin was
 * held.  The boot screen says so: a machine running on defaults because
 * somebody asked it to and one running on them because its record is
 * gone are not the same thing, and the difference matters precisely when
 * somebody is trying to get out of a bad setting.
 */

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
