/*
 * rp2350_rtx.c - real-time extension: the runtime on core 1
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * pTOS owns core 0.  Core 1 runs a separate real-time runtime image,
 * found in flash at RTX_IMAGE_ADDR (see include/rtx_rp2350.h).  pTOS
 * starts it at boot and publishes the cookie "_RTX", through which
 * programs start and stop real-time tasks on core 1 (include/rtx.h).
 * The runtime is not part of the pTOS image and may be under any licence;
 * the two only share the contract in rtx_rp2350.h.
 *
 * The rtx_api functions run in the calling program's context (user mode
 * on core 0).  They only touch the mailbox in SRAM and the microsecond
 * timer, and the runtime picks commands up by polling the mailbox.
 */

#include "emutos.h"
#include "rp2350.h"
#include "cookie.h"
#include "rtx.h"
#include "rtx_rp2350.h"
#include "rp2350_rtx.h"

#if CONF_WITH_RP2350_RTX

#define SIO_FIFO_ST     RP2350_REG(RP2350_SIO_BASE + 0x50)
#define SIO_FIFO_WR     RP2350_REG(RP2350_SIO_BASE + 0x54)
#define SIO_FIFO_RD     RP2350_REG(RP2350_SIO_BASE + 0x58)
#define SIO_FIFO_VLD    0x1UL
#define SIO_FIFO_RDY    0x2UL

#define TIMER0_TIMERAWL RP2350_REG(RP2350_TIMER0_BASE + 0x28)

#define START_TIMEOUT_US    200000UL    /* runtime must come up within */
#define CMD_TIMEOUT_US      100000UL    /* and answer a command within */

#define image   ((const struct rtx_image *)RTX_IMAGE_ADDR)
#define mailbox ((struct rtx_mailbox *)RTX_MAILBOX_ADDR)

static BOOL runtime_up;
static const char *runtime_state;     /* NULL: not started (no .data: it is read-only) */

static ULONG fifo_exchange(ULONG value)
{
    while (!(SIO_FIFO_ST & SIO_FIFO_RDY))
        ;
    SIO_FIFO_WR = value;
    __asm__ volatile ("sev");
    while (!(SIO_FIFO_ST & SIO_FIFO_VLD))
        __asm__ volatile ("wfe");
    return SIO_FIFO_RD;
}

/*
 * Launch core 1 through the bootrom's protocol (RP2350 datasheet, 5.3
 * "Launching code on processor core 1"): core 1 waits in the bootrom for
 * the sequence 0, 0, 1, vector table, stack pointer, entry point on the
 * inter-core FIFO, echoing every word; on a mismatch start over.
 */
static void launch_core1(ULONG vtor, ULONG sp, ULONG entry)
{
    ULONG seq[6];
    int i = 0;

    seq[0] = 0;
    seq[1] = 0;
    seq[2] = 1;
    seq[3] = vtor;
    seq[4] = sp;
    seq[5] = entry;

    while (i < 6)
    {
        if (seq[i] == 0)
        {
            while (SIO_FIFO_ST & SIO_FIFO_VLD)  /* drain stale words */
                (void)SIO_FIFO_RD;
            __asm__ volatile ("sev");
        }
        i = (fifo_exchange(seq[i]) == seq[i]) ? i + 1 : 0;
    }
}

/* one command through the mailbox; runs in the calling program's context */
static long rtx_call(ULONG cmd, ULONG a0, ULONG a1, ULONG a2, ULONG a3)
{
    ULONG seq, start;

    if (!runtime_up || mailbox->magic != RTX_MAILBOX_MAGIC)
        return RTX_E_NOCORE;

    mailbox->cmd = cmd;
    mailbox->args[0] = a0;
    mailbox->args[1] = a1;
    mailbox->args[2] = a2;
    mailbox->args[3] = a3;
    seq = mailbox->seq + 1;
    __asm__ volatile ("dmb" ::: "memory");     /* command before seq */
    mailbox->seq = seq;

    start = TIMER0_TIMERAWL;
    while (mailbox->done != seq)
    {
        if (TIMER0_TIMERAWL - start > CMD_TIMEOUT_US)
            return RTX_E_TIMEOUT;
    }
    __asm__ volatile ("dmb" ::: "memory");

    return mailbox->result;
}

static long rtx_start(rtx_func init, rtx_func cyclic, void *arg, unsigned long period_us)
{
    if (!cyclic || period_us < 100)
        return RTX_E_BADARG;
    return rtx_call(RTX_CMD_START, (ULONG)init, (ULONG)cyclic, (ULONG)arg, period_us);
}

static long rtx_stop(long task)
{
    return rtx_call(RTX_CMD_STOP, (ULONG)task, 0, 0, 0);
}

static long rtx_status(struct rtx_status *st)
{
    long rc = rtx_call(RTX_CMD_STATUS, 0, 0, 0, 0);
    int i;

    if (rc < 0)
        return rc;
    for (i = 0; i < 6; i++)
        ((ULONG *)st)[i] = mailbox->status[i];
    return 0;
}

static const struct rtx_api rtx_api = {
    RTX_API_VERSION,
    sizeof(struct rtx_api),
    rtx_start,
    rtx_stop,
    rtx_status
};

/* for the welcome screen */
const char *rp2350_rtx_name(void)
{
    return runtime_state ? runtime_state : "none";
}

/* called when the cookie jar is filled */
void rp2350_rtx_init(void)
{
    ULONG start;

    if (image->magic != RTX_IMAGE_MAGIC)
    {
        KINFO(("rtx: no real-time runtime in flash at %08lx\n", RTX_IMAGE_ADDR));
        runtime_state = "none (no runtime image in flash)";
        return;
    }

    mailbox->magic = 0;
    launch_core1(image->vector_table, image->stack_top, image->entry);

    start = TIMER0_TIMERAWL;
    while (mailbox->magic != RTX_MAILBOX_MAGIC)
    {
        if (TIMER0_TIMERAWL - start > START_TIMEOUT_US)
        {
            KINFO(("rtx: runtime \"%s\" did not come up\n", image->name));
            runtime_state = "runtime failed to start";
            return;
        }
    }

    runtime_up = TRUE;
    runtime_state = image->name;
    cookie_add(RTX_COOKIE, (ULONG)&rtx_api);
    KINFO(("rtx: runtime \"%s\" running on core 1\n", image->name));
}

#endif /* CONF_WITH_RP2350_RTX */
