/*
 * gemdisp.h - header for EmuTOS AES dispatcher
 *
 * Copyright (C) 2002-2021 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef GEMDISP_H
#define GEMDISP_H

#include "struct.h"

WORD forkq(FCODE fcode, LONG fdata);
void forker(void);
void chkkbd(void);

void dsptch(void);              /* give the processor away */
void aes_wake(AESPD *p);        /* p has what it waited for */
void aes_sched_init(AESPD *p0);
void aes_sched_exit(void);

#endif
