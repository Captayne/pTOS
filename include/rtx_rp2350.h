/*
 * rtx_rp2350.h - contract between pTOS and the RP2350 real-time runtime
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * MIT licence, see include/rtx.h.  This header is shared by pTOS (which
 * starts the runtime on core 1 and forwards the rtx_api calls of
 * programs to it) and by the runtime itself, which is a separate image
 * that may be under any licence.
 *
 * Layout:
 *   flash 0x10800000  runtime image, starting with struct rtx_image
 *   SRAM  0x20072000  64 KB for core 1: struct rtx_mailbox first, then
 *                     whatever the runtime wants; its stack at the top
 *                     (0x20082000, the end of SRAM9)
 * pTOS uses the SRAM below 0x20072000 only.
 */

#ifndef RTX_RP2350_H
#define RTX_RP2350_H

#define RTX_IMAGE_ADDR      0x10800000UL
#define RTX_RAM_BASE        0x20072000UL
#define RTX_RAM_END         0x20082000UL
#define RTX_MAILBOX_ADDR    RTX_RAM_BASE

#define RTX_IMAGE_MAGIC     0x31585452UL    /* "RTX1" */
#define RTX_MAILBOX_MAGIC   0x42585452UL    /* "RTXB" */

#ifndef __ASSEMBLER__

/* at RTX_IMAGE_ADDR */
struct rtx_image {
    unsigned long magic;            /* RTX_IMAGE_MAGIC */
    unsigned long version;
    unsigned long vector_table;     /* core 1's vector table */
    unsigned long stack_top;        /* core 1's initial main stack */
    unsigned long entry;            /* core 1's entry point (Thumb bit set) */
    char          name[44];         /* NUL terminated */
};

/* mailbox commands (pTOS -> runtime) */
#define RTX_CMD_NONE    0
#define RTX_CMD_START   1   /* args: init, cyclic, arg, period_us */
#define RTX_CMD_STOP    2   /* args: task */
#define RTX_CMD_STATUS  3   /* result in status */

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
};

#endif /* __ASSEMBLER__ */

#endif /* RTX_RP2350_H */
