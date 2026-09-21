/*
 * gemasm.h - header for EmuTOS AES assembler functions
 *
 * Copyright (C) 2002-2020 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef GEMASM_H
#define GEMASM_H

/* launches the top of rlr list, as if called from within function
 * back(AESPD *top_of_rlr)
 */
extern void gotopgm(void) /*NORETURN*/ ;

#endif
