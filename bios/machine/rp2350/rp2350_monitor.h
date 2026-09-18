/*
 * rp2350_monitor.h - bring-up monitor: a heartbeat NMI
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef RP2350_MONITOR_H
#define RP2350_MONITOR_H

void rp2350_monitor_init(void);
void rp2350_monitor_nmi(ULONG *frame);

#endif /* RP2350_MONITOR_H */
