/*
 * lowmem.h - base address of the emulated 68k low memory (RP2350)
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Address 0 is the bootrom on the RP2350 and there is no MMU to remap it,
 * so ST-RAM (and with it the emulated 68k vector table and system
 * variables area) starts at the beginning of the on-chip SRAM.  See
 * bios/arch/arm/lowmem.h and emutos.ld.
 */

#ifndef LOWMEM_H
#define LOWMEM_H

#define LOWMEM_BASE 0x20000000

#endif /* LOWMEM_H */
