/*
 * memory.c - RP2350 memory initialization
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "config.h"

#ifndef MACHINE_RP2350
#error This file must only be compiled for the RP2350 target
#endif

/*
 * bios/build.mk lists memory.o unconditionally, and vpath resolves it to
 * this file for MACHINE_RP2350.  The on-chip SRAM needs no runtime
 * initialization: startup.S clears it and sets phystop.  The PSRAM set-up
 * will live here once the chip is fitted.
 */
