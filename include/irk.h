/*
 * irk.h - multitasking for programs: tasks, semaphores, queues, deadlines
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This header is available under the MIT licence (unlike the rest of
 * pTOS, which is GPL), so that programs of any licence can use it:
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
 * TOS has no threads, no semaphores, no queues and no deadlines.  Where a
 * multitasking kernel is present, this is how a program reaches it.
 *
 * A program finds it through the cookie "_IRK":
 *
 *     long value;
 *     struct irk_api *k = 0;
 *     if (Ssystem(S_GETCOOKIE, IRK_COOKIE, (long)&value) == 0)
 *         k = (struct irk_api *)value;
 *
 * No cookie means no kernel -- the program runs as it always did.  Check
 * once at startup and keep the pointer; check `version` and `size` before
 * using anything added after version 1.
 *
 * NOT EVERY CALL EXISTS EVERYWHERE
 *
 * A pointer in this structure may be **null**, and that is the answer to
 * "can I do this here", not a fault.  Seen from the system core, a call
 * that would wait -- sema_wait(), queue_send(), queue_recv() -- is null,
 * because waiting there is the AES' business: a GEM program waits in
 * evnt_multi(), and a wait ordered across to the real-time core would
 * stop the runtime that serves the request.  yield() and delay_us() are
 * null there for the same reason, and so is notify(), which a headless
 * task uses to reach *upwards*.
 *
 * Inside a task on the real-time core everything is there.  Ask core()
 * if you want to know which side you are on, or simply test the pointer
 * you are about to use.
 *
 * THE TWO HALVES
 *
 * A program is one GEM task on the system core.  If it wants a second
 * half, it creates a *headless* task on the real-time core: no AES, no
 * VDI, no GEMDOS, no console -- it computes, drives hardware and keeps
 * deadlines.  Both halves see the program's own memory, so a pointer to
 * a structure is all they need to share.
 *
 * A headless task cannot draw.  To reach the user interface it calls
 * notify(), and the GEM half receives an ordinary AES message in
 * evnt_multi(MU_MESAG) -- see IRK_MSG below.
 *
 * WHAT A HEADLESS TASK MUST NOT DO
 *
 * Call GEMDOS, BIOS, XBIOS, VDI or AES.  The operating system belongs to
 * the system core.  This is the same rule real-time code on the
 * real-time core has always followed.
 *
 * LIFETIME
 *
 * Tasks, semaphores and queues belong to the program that created them
 * and are destroyed when it ends.  Their stacks and storage are the
 * program's own memory, which the system reclaims -- a task outliving its
 * program would be running on freed memory.  A program that ends tidily
 * still ends tidily; nothing has to be released by hand.
 *
 * WAITING
 *
 * Never wait by spinning:
 *
 *     while (!done) k->yield();        // wrong
 *
 * A task that circles like this counts as runnable and claims its share
 * of the processor, which comes out of everyone else's.  Wait with
 * delay_us(), a semaphore or a queue: on waking, the task is placed at
 * the current level with nothing owed and nothing to claim back.
 */

#ifndef IRK_H
#define IRK_H

#define IRK_COOKIE      0x5F49524BL     /* '_IRK' */
#define IRK_API_VERSION 1

/*
 * Tasks, semaphores and queues are opaque: a number the kernel hands out
 * and recognises.  IRK_NONE is never a valid one, and is what every
 * creating call returns on failure.
 */
typedef unsigned short irk_handle;

#define IRK_NONE        0

/* A task body.  It is called once with the argument given at creation.
 * Returning from it ends the task. */
typedef void (*irk_entry)(void *arg);

/*
 * Which core a task runs on.  The system core carries pTOS; the
 * real-time core carries nothing else, which is why deadlines hold there.
 */
#define IRK_CORE_SYSTEM 0
#define IRK_CORE_RT     1

/*
 * Priority is a *share*, not a rank: a task of priority 200 gets twice
 * the processor time of one at 100 when both want it, and neither
 * starves the other.  1 is the smallest useful share.
 */
#define IRK_PRIO_MIN    1

/*
 * The message notify() delivers to the GEM half.  Standard AES message
 * layout: msg[0] is the number below, msg[1] the sender (0 -- it does
 * not come from a GEM application), msg[2] the extra length (0).
 * msg[3] and msg[4] carry the sending task's handle, and msg[5]..msg[7]
 * the two longs passed to notify(), high word first.
 *
 * The number is far outside the range the AES uses for itself.
 */
#define IRK_MSG         0x4952          /* 'IR' */

/*
 * Return values.  Calls that can fail return 0 for success and a
 * negative number otherwise; calls that create something return a handle
 * or IRK_NONE.
 */
#define IRK_OK          0L
#define IRK_ERR        (-1L)            /* refused: bad handle, not yours,
                                           nothing free, wrong core */

struct irk_api {
    unsigned short  version;            /* IRK_API_VERSION */
    unsigned short  size;               /* sizeof(struct irk_api) */

    /*--- where am I ------------------------------------------------*/

    /* The core this call runs on: IRK_CORE_SYSTEM or IRK_CORE_RT. */
    unsigned short (*core)(void);

    /* How many cores the kernel carries.  1 means there is no real-time
       core to put a headless task on. */
    unsigned short (*cores)(void);

    /*--- tasks -----------------------------------------------------*/

    /*
     * Create a task on `core` and make it runnable.
     *
     *   fn     the body; it gets `arg` and must not return except to end
     *   prio   share of the processor, IRK_PRIO_MIN upwards
     *   stack  memory for its stack, or 0 to let the kernel serve it.
     *          On the real-time core the kernel's own pool is used and
     *          this is ignored: that memory is in the faster bank, and
     *          it cannot be taken away when your program's memory is
     *          reclaimed.  The parameter is kept for the day a task can
     *          run on the system core, where your own memory is right.
     *   size   how much stack the task needs, in bytes, or 0 for the
     *          default.  A headless task needs little; find out with
     *          stack_free() rather than guessing generously.
     *   name   up to 8 characters for diagnostics, or 0.  Not carried
     *          across from the system core: the text would have to stay
     *          put for the life of the task.
     *
     * Returns the handle, or IRK_NONE if the core has no room, the stack
     * is too small, or there is no such core.
     */
    irk_handle (*task_new)(unsigned short core, irk_entry fn, void *arg,
                           unsigned short prio,
                           void *stack, unsigned long size,
                           const char *name);

    /* End a task.  A task may end itself, in which case this does not
       return.  Its stack is the program's to reuse afterwards. */
    long (*task_kill)(irk_handle t);

    /* Stop a task without ending it, and let it run again.  A suspended
       task keeps its place: it is not owed the time it missed. */
    long (*task_suspend)(irk_handle t);
    long (*task_resume)(irk_handle t);

    /* The calling task. */
    irk_handle (*task_self)(void);

    /* Change a task's share.  Takes effect at the next switch. */
    long (*set_prio)(irk_handle t, unsigned short prio);
    long (*get_prio)(irk_handle t);

    /*
     * Run a task every `period_us` microseconds instead of sharing the
     * processor.  It runs, returns, and is started again at the next due
     * time -- not `period_us` after it finished, so the period does not
     * drift.  `start_after_us` delays the first run; 0 starts it at the
     * next due point.
     *
     * A cyclic task is not woken while it waits for something else, so
     * it never re-enters itself.
     */
    long (*set_cyclic)(irk_handle t, unsigned long period_us,
                       unsigned long start_after_us);

    /* Back to sharing the processor, with the given priority. */
    long (*set_normal)(irk_handle t, unsigned short prio);

    /*--- giving way and time ---------------------------------------*/

    /* "Done for now."  Checks whether anything else is more owed, and
       switches if so.  Returns at once when nothing is. */
    void (*yield)(void);

    /* Wait.  The task is not runnable meanwhile and claims nothing. */
    void (*delay_us)(unsigned long us);

    /* Microseconds since the kernel started, low 32 bits.  Wraps every
       71 minutes or so; differences computed in unsigned arithmetic stay
       exact across the wrap, which is how the kernel itself measures. */
    unsigned long (*now_us)(void);

    /*--- semaphores ------------------------------------------------*/

    /*
     * Take one from the pool.  `count` is what it starts at: 1 for a
     * mutual exclusion, 0 for "wait until someone signals", n to admit n
     * at a time.  Freed automatically when the program ends.
     */
    irk_handle (*sema_new)(long count);
    long (*sema_free)(irk_handle s);

    /* Wait until it is free and take it; try without waiting; give it
       back.  sema_try() returns IRK_OK when it got it. */
    long (*sema_wait)(irk_handle s);
    long (*sema_try)(irk_handle s);
    long (*sema_signal)(irk_handle s);

    /*--- queues ----------------------------------------------------*/

    /*
     * A queue of fixed-size items, in memory the program provides:
     * `storage` must hold items * itemsize bytes and stay put for the
     * queue's life.  Items are copied in and out, so sender and receiver
     * never share a buffer.
     */
    irk_handle (*queue_new)(void *storage, unsigned short items,
                            unsigned short itemsize);
    long (*queue_free)(irk_handle q);

    /* Send and receive, waiting for room or for an item. */
    long (*queue_send)(irk_handle q, const void *item);
    long (*queue_recv)(irk_handle q, void *item);

    /* The same without waiting: IRK_OK when it worked. */
    long (*queue_try_send)(irk_handle q, const void *item);
    long (*queue_try_recv)(irk_handle q, void *item);

    /* How many items are waiting. */
    long (*queue_count)(irk_handle q);

    /*--- telling the other half ------------------------------------*/

    /*
     * From a headless task: send an IRK_MSG to the GEM half of this
     * program, which receives it in evnt_multi(MU_MESAG).
     *
     * Notifications are *coalesced*: while one is undelivered, further
     * calls update the payload instead of queueing another.  A task that
     * reports every millisecond therefore cannot flood the AES, and the
     * GEM half always sees the newest values.  Put the real data in
     * shared memory and use the two longs for "what happened".
     *
     * Delivery happens at the system core's next dispatch, within about
     * a millisecond.  Returns IRK_ERR if the program has no GEM half
     * any more.
     */
    long (*notify)(unsigned long a, unsigned long b);

    /*--- what is going on ------------------------------------------*/

    /* Bytes of the task's stack never touched.  For sizing stacks with
       measurements instead of superstition. */
    long (*stack_free)(irk_handle t);

    /* Microseconds this task has had the processor, low 32 bits. */
    unsigned long (*runtime_us)(irk_handle t);

    /*
     * The same interface as it exists *on the real-time core*.
     *
     * A headless task cannot use the pointers above: those were obtained
     * on the system core and every one of them sends a request across to
     * the real-time core.  Used from over there they would be a call to
     * oneself, by way of the mailbox one is meant to be serving.
     *
     * So a program fetches this pointer here, hands it to its task --
     * through the argument, or in the memory both halves share -- and
     * the task uses that one.  In it nothing is missing: waiting is
     * exactly what a task over there is allowed to do.
     *
     * Null when there is no real-time core.
     */
    struct irk_api *(*rt_api)(void);
};

#endif /* IRK_H */
