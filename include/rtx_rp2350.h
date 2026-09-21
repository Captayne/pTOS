/*
 * rtx_rp2350.h - contract between pTOS and the RP2350 real-time runtime
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * MIT licence, see include/rtx.h.  This header is shared by pTOS and by
 * the kernel it runs on, which is a separate image that may be under any
 * licence.
 *
 * The kernel boots.  It sits at the start of flash, where the bootrom
 * looks, starts core 1 for itself and then enters pTOS on core 0 through
 * the vector table at PTOS_IMAGE_ADDR.  pTOS forwards the rtx_api calls
 * of programs to it.
 *
 * Layout:
 *   flash 0x10000000  kernel image; struct rtx_image at RTX_IMAGE_ADDR
 *   flash 0x10010000  pTOS (ROM_ORIGIN in emutos.ld says the same)
 *   SRAM  0x20072000  64 KB for the kernel: struct rtx_mailbox first,
 *                     then its code and data; core 1's stack at the top
 *                     (0x20082000, the end of SRAM9)
 * pTOS uses the SRAM below 0x20072000 only.
 */

#ifndef RTX_RP2350_H
#define RTX_RP2350_H

#define KERNEL_IMAGE_ADDR   0x10000000UL
#define RTX_IMAGE_ADDR      0x10000400UL
#define PTOS_IMAGE_ADDR     0x10010000UL
#define RTX_RAM_BASE        0x20072000UL
#define RTX_RAM_END         0x20082000UL
#define RTX_MAILBOX_ADDR    RTX_RAM_BASE

#define RTX_IMAGE_MAGIC     0x32585452UL    /* "RTX2" */
#define RTX_MAILBOX_MAGIC   0x42585452UL    /* "RTXB" */

#ifndef __ASSEMBLER__

/* at RTX_IMAGE_ADDR */
struct rtx_image {
    unsigned long magic;            /* RTX_IMAGE_MAGIC */
    unsigned long version;
    char          name[44];         /* NUL terminated */
    const struct kernel_api *api;   /* in the kernel's SRAM once it runs */
};

/*
 * What pTOS calls in the kernel: plain function calls on core 0, in the
 * caller's context.  Append, never reorder.
 */
struct kernel_api {
    unsigned long version;
    unsigned long size;             /* sizeof(struct kernel_api) */

    /* Start core 1.  Called once, when pTOS has set up the clocks and
       the timer.  0 on success, -1 when core 1 did not answer. */
    long (*start_core1)(void);
};

/* mailbox commands (pTOS -> runtime) */
#define RTX_CMD_NONE    0
#define RTX_CMD_START   1   /* args: init, cyclic, arg, period_us */
#define RTX_CMD_STOP    2   /* args: task */
#define RTX_CMD_STATUS  3   /* result in status */

/*
 * The rest carry the _IRK interface (include/irk.h) across to the
 * real-time core.  These numbers are the ABI between the two images:
 * append, never renumber.
 *
 * Only what makes sense from outside is here.  A call that waits --
 * sema_wait(), queue_send(), queue_recv() -- belongs to tasks running on
 * the real-time core itself; asking for it from the system core would
 * block the runtime's own main task, which is what serves this mailbox.
 * The system core uses the try_ forms and lets notify() tell it when
 * there is something to look at.
 */
#define RTX_CMD_TASK_NEW    4   /* args: fn, arg, prio, stack size */
#define RTX_CMD_TASK_KILL   5   /* args: handle */
#define RTX_CMD_TASK_CTL    6   /* args: handle, RTX_CTL_*, value */
#define RTX_CMD_TASK_CYCLIC 7   /* args: handle, period_us, start_after_us */
#define RTX_CMD_SEMA_NEW    8   /* args: initial count */
#define RTX_CMD_SEMA_FREE   9   /* args: handle */
#define RTX_CMD_SEMA_OP    10   /* args: handle, RTX_SEM_* */
#define RTX_CMD_QUEUE_NEW  11   /* args: storage, items, item size */
#define RTX_CMD_QUEUE_FREE 12   /* args: handle */
#define RTX_CMD_QUEUE_OP   13   /* args: handle, RTX_Q_*, item pointer */

/* RTX_CMD_TASK_CTL */
#define RTX_CTL_SUSPEND  0
#define RTX_CTL_RESUME   1
#define RTX_CTL_SET_PRIO 2
#define RTX_CTL_GET_PRIO 3
#define RTX_CTL_NORMAL   4      /* leave cyclic operation, value = prio */
#define RTX_CTL_STACK    5      /* bytes of stack never touched */
#define RTX_CTL_RUNTIME  6      /* microseconds of processor had, low 32 */

/* RTX_CMD_SEMA_OP */
#define RTX_SEM_SIGNAL   0
#define RTX_SEM_TRY      1
#define RTX_SEM_COUNT    2

/* RTX_CMD_QUEUE_OP */
#define RTX_Q_TRY_SEND   0
#define RTX_Q_TRY_RECV   1
#define RTX_Q_COUNT      2

/* at RTX_MAILBOX_ADDR; written by the runtime at start-up */
struct rtx_mailbox {
    volatile unsigned long magic;       /* RTX_MAILBOX_MAGIC once running */
    volatile unsigned long version;
    /* one command at a time: pTOS fills cmd/args, increments seq and
     * rings the doorbell (SIO FIFO); the runtime executes it, stores the
     * result and sets done = seq */
    volatile unsigned long seq;
    volatile unsigned long done;
    volatile unsigned long cmd;
    volatile unsigned long args[4];
    volatile long          result;
    volatile unsigned long status[6];   /* struct rtx_status, for STATUS */
    volatile unsigned long fault[4];    /* runtime fault: ipsr, pc, lr, cfsr */
    /* struct irk_api as it exists on the real-time core: the one a
     * headless task uses, since the system core's binding would
     * only send it back across the mailbox.  Written at start-up. */
    volatile unsigned long api;
    /* One notification from a headless task, waiting to be picked
     * up by the system core.  Coalesced on purpose: while note is
     * set, a further notify() replaces the payload instead of
     * queueing another, so a task reporting every millisecond can
     * neither flood the AES nor lose the newest values. */
    volatile unsigned long note;        /* 0 = nothing pending */
    volatile unsigned long note_task;
    volatile unsigned long note_a;
    volatile unsigned long note_b;
};

#endif /* __ASSEMBLER__ */

#endif /* RTX_RP2350_H */
