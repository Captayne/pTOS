/*
 * sched_abi.h - the interface between pTOS and whatever schedules it
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This header is available under the MIT licence (unlike the rest of
 * pTOS, which is GPL), so that a scheduler of any licence can implement
 * it:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions: The above copyright notice and this
 * permission notice shall be included in all copies or substantial
 * portions of the Software.  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT
 * WARRANTY OF ANY KIND.
 *
 * pTOS does not schedule itself.  It gives the processor away, and says
 * whether it wants it back at once or only when something wakes it.
 * Everything else -- which task runs next, how contexts are switched,
 * how time is accounted -- belongs behind this header.
 *
 * Nothing of pTOS crosses the line.  No AESPD, no PD, no event block, no
 * ready list: an implementation never learns what a GEM process is, and
 * could as well be scheduling a CNC controller.  That is not politeness,
 * it is the point.  An interface shaped around one implementation, or
 * named after it, is not an interface.
 *
 * Two implementations:
 *
 *   sched_aes.c   the dispatcher pTOS has always had.  The default, and
 *                 complete: pTOS needs nothing else to run.
 *   sched_irk.c   IRKernel -- a separately licensed component that is
 *                 not part of pTOS.  See docs/scheduler-abi.md.
 *
 * What is covered here is the scheduling half: give away, wait, who am
 * I.  Creating and ending tasks still belongs to the AES, which builds
 * its processes with their own stacks and contexts, and waking still
 * goes through its fork ring.  Both move behind this header once a
 * backend owns the context switch -- there is no point in abstracting
 * them while only one implementation exists.
 */

#ifndef SCHED_ABI_H
#define SCHED_ABI_H

/*
 * A task, as far as this interface is concerned: an opaque number that
 * the implementation hands out and recognises.  Zero is never a task.
 */
typedef unsigned long k_task_t;

/*
 * Why a task stopped being runnable.  An implementation need not act on
 * this beyond "not runnable until woken"; it exists so that a diagnostic
 * can say what a task is waiting for.
 */
#define K_WAIT_EVENT    1u      /* a message, a key, a timer, ... */

/*
 * Give the processor away and stay runnable.  Returns once the caller
 * has it back, which may be immediately.
 */
void k_yield(void);

/*
 * Give the processor away and stop being runnable.  Returns only after
 * something has made the caller runnable again.
 */
void k_block(unsigned long reason);

/*
 * The task that is running.  Never zero while pTOS is running.
 */
k_task_t k_current(void);

#endif /* SCHED_ABI_H */
