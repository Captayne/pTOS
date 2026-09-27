/*
 * delay.h - header for delay.c
 *
 * Copyright (C) 2013-2019 The EmuTOS development team
 *
 * Authors:
 *  RFB    Roger Burrows
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef _DELAY_H
#define _DELAY_H

/*
 * this is the value to pass to the inline function delay_loop()
 * to get a delay of 1 millisecond.  other delays may be obtained
 * by multiplying or dividing as appropriate.  when calculating
 * shorter delays, rounding up is not necessary: because of the
 * instructions used in the loop (see asm.h), the number of loops
 * executed is one more than this count (iff count >= 0).
 */
extern ULONG loopcount_1_msec;

/*
 * Microseconds from a counter nobody has to maintain.
 *
 * hz_200 is the system clock and is the right thing to measure by in a
 * task -- but it is incremented by the timer interrupt, so inside any
 * other interrupt handler on a machine whose interrupts do not preempt
 * each other it stands still, and a deadline computed from it is never
 * reached.  A driver waiting that way waits for a hand it is itself
 * holding.
 *
 * Where the hardware has a free-running counter this returns it, and a
 * deadline works wherever it is set.  Where it does not, this is hz_200
 * converted, which is no worse than what the caller would have written.
 *
 * It wraps.  Compare differences, never values:
 *
 *     ULONG end = monotonic_usec() + microseconds;
 *     while ((LONG)(monotonic_usec() - end) < 0)
 *         ...
 */
ULONG monotonic_usec(void);

/*
 * function prototypes
 */
void init_delay(void);
void calibrate_delay(void);

#endif  /* _DELAY_H */
