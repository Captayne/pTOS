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
#include "irk.h"
#include "extmsg.h"
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

/*========================================================================*\
 *  _IRK: the multitasking interface, as it reaches the system core
 *
 *  Every call here is a mailbox command, so a program on this core can
 *  create, steer and question tasks on the real-time core, and exchange
 *  items with them -- but it never waits on the kernel.  Waiting belongs
 *  to the tasks over there; a wait ordered from here would block the
 *  runtime's own main task, which is what serves this mailbox.
 *
 *  Calls that would wait are therefore NULL in this binding, and so are
 *  the ones that only make sense inside a task.  A program checks a
 *  pointer before it uses it, or asks core() first.
 *
 *  One mailbox serves everyone.  That is safe while the AES switches
 *  cooperatively and nothing in irk_call() gives way -- the same reason
 *  a GEMDOS call is atomic today.  When the system core gains real
 *  multitasking, this needs a lock, and that is the moment to add one:
 *  not before, when it would only be untested weight.
\*========================================================================*/

static long irk_call(ULONG cmd, ULONG a0, ULONG a1, ULONG a2, ULONG a3)
{
    return rtx_call(cmd, a0, a1, a2, a3);
}

static unsigned short irk_core(void)
{
    return IRK_CORE_SYSTEM;
}

static unsigned short irk_cores(void)
{
    return runtime_up ? 2 : 1;
}

static irk_handle irk_task_new(unsigned short core, irk_entry fn, void *arg,
                               unsigned short prio,
                               void *stack, unsigned long size,
                               const char *name)
{
    long rc;

    /* Tasks on the system core would each need their own GEMDOS context;
       see docs/multitasking.md.  Until then, the real-time core only. */
    if (core != IRK_CORE_RT)
        return IRK_NONE;

    /* The stack comes from the real-time core's own pool: it sits in the
       fast bank, and it cannot be pulled away when this program's memory
       is reclaimed.  The size is passed so that the runtime can refuse a
       request its pool cannot serve; a name would have to stay valid for
       the life of the task, so it is not carried across. */
    (void)stack;
    (void)name;

    rc = irk_call(RTX_CMD_TASK_NEW, (ULONG)fn, (ULONG)arg, prio, size);
    return (rc > 0) ? (irk_handle)rc : IRK_NONE;
}

static long irk_task_kill(irk_handle t)
{
    return irk_call(RTX_CMD_TASK_KILL, t, 0, 0, 0);
}

static long irk_task_suspend(irk_handle t)
{
    return irk_call(RTX_CMD_TASK_CTL, t, RTX_CTL_SUSPEND, 0, 0);
}

static long irk_task_resume(irk_handle t)
{
    return irk_call(RTX_CMD_TASK_CTL, t, RTX_CTL_RESUME, 0, 0);
}

static long irk_set_prio(irk_handle t, unsigned short prio)
{
    return irk_call(RTX_CMD_TASK_CTL, t, RTX_CTL_SET_PRIO, prio, 0);
}

static long irk_get_prio(irk_handle t)
{
    return irk_call(RTX_CMD_TASK_CTL, t, RTX_CTL_GET_PRIO, 0, 0);
}

static long irk_set_cyclic(irk_handle t, unsigned long period_us,
                           unsigned long start_after_us)
{
    return irk_call(RTX_CMD_TASK_CYCLIC, t, period_us, start_after_us, 0);
}

static long irk_set_normal(irk_handle t, unsigned short prio)
{
    return irk_call(RTX_CMD_TASK_CTL, t, RTX_CTL_NORMAL, prio, 0);
}

static unsigned long irk_now_us(void)
{
    return TIMER0_TIMERAWL;
}

static irk_handle irk_sema_new(long count)
{
    long rc = irk_call(RTX_CMD_SEMA_NEW, (ULONG)count, 0, 0, 0);

    return (rc > 0) ? (irk_handle)rc : IRK_NONE;
}

static long irk_sema_free(irk_handle s)
{
    return irk_call(RTX_CMD_SEMA_FREE, s, 0, 0, 0);
}

static long irk_sema_try(irk_handle s)
{
    return irk_call(RTX_CMD_SEMA_OP, s, RTX_SEM_TRY, 0, 0);
}

static long irk_sema_signal(irk_handle s)
{
    return irk_call(RTX_CMD_SEMA_OP, s, RTX_SEM_SIGNAL, 0, 0);
}

static irk_handle irk_queue_new(void *storage, unsigned short items,
                                unsigned short itemsize)
{
    long rc = irk_call(RTX_CMD_QUEUE_NEW, (ULONG)storage, items, itemsize, 0);

    return (rc > 0) ? (irk_handle)rc : IRK_NONE;
}

static long irk_queue_free(irk_handle q)
{
    return irk_call(RTX_CMD_QUEUE_FREE, q, 0, 0, 0);
}

static long irk_queue_try_send(irk_handle q, const void *item)
{
    return irk_call(RTX_CMD_QUEUE_OP, q, RTX_Q_TRY_SEND, (ULONG)item, 0);
}

static long irk_queue_try_recv(irk_handle q, void *item)
{
    return irk_call(RTX_CMD_QUEUE_OP, q, RTX_Q_TRY_RECV, (ULONG)item, 0);
}

static long irk_queue_count(irk_handle q)
{
    return irk_call(RTX_CMD_QUEUE_OP, q, RTX_Q_COUNT, 0, 0);
}

static long irk_stack_free(irk_handle t)
{
    return irk_call(RTX_CMD_TASK_CTL, t, RTX_CTL_STACK, 0, 0);
}

/*
 *  Where notifications go.  A program says so once, after appl_init(),
 *  with the id that call gave it: the runtime cannot know which GEM
 *  application a headless task belongs to, and guessing would be worse
 *  than asking.  -1 means nobody is listening, and a notification is
 *  then dropped rather than kept for whoever comes along next.
 */
static WORD notify_pid = -1;

static long irk_notify_to(unsigned short apid)
{
    notify_pid = (WORD)apid;
    return IRK_OK;
}

/*
 *  Called by the AES from its dispatcher, never from an interrupt: it
 *  asks whether a device has a message for an application.  Returns the
 *  process id, or -1 when there is nothing.
 */
static WORD irk_extmsg(WORD *msg)
{
    ULONG task, a, b;

    if (!runtime_up || !mailbox->note)
        return -1;

    task = mailbox->note_task;
    a = mailbox->note_a;
    b = mailbox->note_b;
    __asm__ volatile ("dmb" ::: "memory");
    mailbox->note = 0;                  /* room for the next one */

    if (notify_pid < 0)
        return -1;                      /* nobody asked for these */

    msg[0] = IRK_MSG;
    msg[1] = 0;                         /* not from a GEM application */
    msg[2] = 0;                         /* no extra length */
    msg[3] = (WORD)task;
    msg[4] = (WORD)(a >> 16);
    msg[5] = (WORD)a;
    msg[6] = (WORD)(b >> 16);
    msg[7] = (WORD)b;
    return notify_pid;
}

static struct irk_api *irk_rt_api(void)
{
    if (!runtime_up || mailbox->magic != RTX_MAILBOX_MAGIC)
        return NULL;
    return (struct irk_api *)mailbox->api;
}

static unsigned long irk_runtime_us(irk_handle t)
{
    long rc = irk_call(RTX_CMD_TASK_CTL, t, RTX_CTL_RUNTIME, 0, 0);

    return (rc < 0) ? 0UL : (unsigned long)rc;
}

static const struct irk_api irk_api = {
    IRK_API_VERSION,
    sizeof(struct irk_api),

    irk_core,
    irk_cores,

    irk_task_new,
    irk_task_kill,
    irk_task_suspend,
    irk_task_resume,
    NULL,                       /* task_self: the caller is not a task  */
    irk_set_prio,
    irk_get_prio,
    irk_set_cyclic,
    irk_set_normal,

    NULL,                       /* yield:    use the AES on this core   */
    NULL,                       /* delay_us: use evnt_timer()           */
    irk_now_us,

    irk_sema_new,
    irk_sema_free,
    NULL,                       /* sema_wait: would block the runtime   */
    irk_sema_try,
    irk_sema_signal,

    irk_queue_new,
    irk_queue_free,
    NULL,                       /* queue_send: would block the runtime  */
    NULL,                       /* queue_recv: would block the runtime  */
    irk_queue_try_send,
    irk_queue_try_recv,
    irk_queue_count,

    NULL,                       /* notify: from a headless task upwards */
    irk_notify_to,

    irk_stack_free,
    irk_runtime_us,

    irk_rt_api
};


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
    cookie_add(IRK_COOKIE, (ULONG)&irk_api);
    aes_extmsg = irk_extmsg;
    KINFO(("rtx: runtime \"%s\" running on core 1\n", image->name));
}

#endif /* CONF_WITH_RP2350_RTX */
