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
 * pTOS does not schedule itself.  It runs on a kernel, and every AES
 * process is one of the kernel's tasks: pTOS creates them on stacks it
 * provides, gives the processor away, waits, and wakes whoever an event
 * is for.  Which task runs next, how contexts are switched and how time
 * is accounted belongs to the kernel.
 *
 * Nothing of pTOS crosses the line.  No AESPD, no PD, no event block: the
 * kernel never learns what a GEM process is, and could as well be
 * scheduling a CNC controller.  That is not politeness, it is the point.
 *
 * On the RP2350 the kernel is IRKernel, a separate image that boots and
 * runs pTOS; bios/machine/rp2350/rp2350_sched.c binds these calls to its
 * call table (include/rtx_rp2350.h).
 */

#ifndef SCHED_ABI_H
#define SCHED_ABI_H

/*
 * A task, as far as this interface is concerned: an opaque number that
 * the kernel hands out and recognises.  Zero is never a task.
 */
typedef unsigned long k_task_t;

/*
 * A new task, runnable at once, running entry() on the given stack.
 * Zero when the kernel has no room for another.
 */
k_task_t k_task_create(void (*entry)(void), void *stack, unsigned long size);

/* End a task that is not the caller. */
void k_task_kill(k_task_t task);

/*
 * Give the processor away and stay runnable.  Returns once the caller
 * has it back, which may be immediately.
 */
void k_yield(void);

/*
 * Give the processor away and stop being runnable.  Returns only after
 * k_wake() has been called for the caller.
 */
void k_block(void);

/* Make a blocked task runnable again. */
void k_wake(k_task_t task);

/* The task that is running.  Never zero. */
k_task_t k_current(void);

/*
 * What the kernel calls whenever no task can run: pTOS collects its input
 * there and wakes whoever it is for.  It sleeps until the next interrupt
 * when it has woken nobody.
 */
void k_set_idle(void (*idle)(void));

#endif /* SCHED_ABI_H */
