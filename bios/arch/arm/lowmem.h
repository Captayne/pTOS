/*
 * lowmem.h - base address of the emulated 68k low memory
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * The 68k-style exception vector table (0x000-0x3ff) and the system
 * variables area (up to 0x800) live at the start of ST-RAM.  On the
 * A-profile ARM machines that is address 0 (raspi) or is mapped there by
 * the MMU (virt-arm).  A machine whose RAM starts elsewhere and that has no
 * MMU to hide it provides its own lowmem.h in its machine directory, which
 * vpath/-I ordering picks up first.
 */

#ifndef LOWMEM_H
#define LOWMEM_H

#define LOWMEM_BASE 0

#endif /* LOWMEM_H */
