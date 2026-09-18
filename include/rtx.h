/*
 * rtx.h - pTOS real-time extension: run code on the real-time core
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This header is available under the MIT licence (unlike the rest of
 * pTOS, which is GPL), so that programs and real-time runtimes of any
 * licence can use it:
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
 * A pTOS program consists of up to two halves:
 *
 *  - the normal half: an ordinary TOS/GEM program on the system core,
 *    with files, console, windows and dialogs;
 *  - the real-time half: functions that run on the real-time core (the
 *    second CPU core, where there is one), called on a fixed period and
 *    never disturbed by whatever the system core is doing.
 *
 * The normal half finds the real-time extension through the cookie
 * "_RTX" (Ssystem(S_GETCOOKIE, RTX_COOKIE, 0)), whose value points to a
 * struct rtx_api.  The halves share memory: data structures in the
 * program's own memory are visible to both, so a real-time task gets a
 * pointer to them as its argument.
 *
 * Real-time functions must not call GEMDOS, BIOS, XBIOS, VDI or AES:
 * the operating system belongs to the system core.  They may access the
 * hardware directly, and communicate with the normal half through shared
 * memory (see rtx_ring below for a simple single-producer/single-consumer
 * queue).
 */

#ifndef RTX_H
#define RTX_H

#define RTX_COOKIE      0x5f525458L     /* '_RTX' */
#define RTX_API_VERSION 1

/* a real-time function; arg is what was passed to rtx_api.start() */
typedef void (*rtx_func)(void *arg);

/* errors (negative return values) */
#define RTX_E_NOCORE    -1L     /* no real-time runtime running */
#define RTX_E_TIMEOUT   -2L     /* the runtime did not answer */
#define RTX_E_NOSLOT    -3L     /* too many real-time tasks */
#define RTX_E_BADARG    -4L     /* invalid argument or task */

struct rtx_status {
    unsigned long   version;        /* runtime version */
    unsigned long   tasks;          /* real-time tasks running */
    unsigned long   uptime_us;      /* runtime clock, low 32 bits */
    unsigned long   max_late_us;    /* worst lateness of a cyclic call
                                     * since the last status query */
    unsigned long   calls;          /* cyclic calls since the last query */
    long            last_error;     /* last runtime error, 0 if none */
};

struct rtx_api {
    unsigned short  version;        /* RTX_API_VERSION */
    unsigned short  size;           /* sizeof(struct rtx_api) */

    /*
     * Start a real-time task on the real-time core.
     *  init        called once on the real-time core before the first
     *              cyclic call (may be NULL), e.g. to set up hardware
     *  cyclic      called every period_us microseconds
     *  arg         passed to init and cyclic
     *  period_us   period of the cyclic calls, >= 100
     * Returns a task number >= 0, or an RTX_E_ error.
     */
    long (*start)(rtx_func init, rtx_func cyclic, void *arg,
                  unsigned long period_us);

    /* Stop a real-time task; it is not called again after this returns. */
    long (*stop)(long task);

    /* Runtime state and timing statistics. */
    long (*status)(struct rtx_status *st);
};

/*
 * A single-producer/single-consumer ring of 32-bit items in shared memory,
 * for passing data between the two halves without any locking: one side
 * only ever writes head, the other only ever writes tail.  size must be a
 * power of two.
 */
struct rtx_ring {
    volatile unsigned long head;    /* written by the producer only */
    volatile unsigned long tail;    /* written by the consumer only */
    unsigned long size;
    unsigned long *items;
};

#define RTX_BARRIER() __asm__ volatile ("dmb" ::: "memory")

static __inline__ int rtx_ring_put(struct rtx_ring *r, unsigned long item)
{
    unsigned long head = r->head;

    if (head - r->tail >= r->size)
        return 0;                   /* full */
    r->items[head & (r->size - 1)] = item;
    RTX_BARRIER();                  /* item before index */
    r->head = head + 1;
    return 1;
}

static __inline__ int rtx_ring_get(struct rtx_ring *r, unsigned long *item)
{
    unsigned long tail = r->tail;

    if (tail == r->head)
        return 0;                   /* empty */
    RTX_BARRIER();                  /* index before item */
    *item = r->items[tail & (r->size - 1)];
    RTX_BARRIER();
    r->tail = tail + 1;
    return 1;
}

static __inline__ unsigned long rtx_ring_count(const struct rtx_ring *r)
{
    return r->head - r->tail;
}

#endif /* RTX_H */
