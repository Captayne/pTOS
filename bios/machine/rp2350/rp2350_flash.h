/*
 * rp2350_flash.h - erasing and programming the QSPI flash
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef RP2350_FLASH_H
#define RP2350_FLASH_H

BOOL rp2350_flash_init(void);
void rp2350_flash_erase(ULONG offs, ULONG count);
void rp2350_flash_program(ULONG offs, const UBYTE *data, ULONG count);

#endif /* RP2350_FLASH_H */
