/*
 * earlyfault.h - report a fault taken before the vector table is set up
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef EARLYFAULT_H
#define EARLYFAULT_H

void armv8m_early_fault(int vector, ULONG *frame, ULONG fsr, ULONG far);
void armv8m_fault_report(int vector, ULONG *frame, ULONG fsr, ULONG far);

/* provided by the machine: raw character output, usable from reset on */
void armv8m_debug_putc(char c);

void halt(void) NORETURN;

#endif /* EARLYFAULT_H */
