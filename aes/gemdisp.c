/*      GEMDISP.C       1/27/84 - 09/13/85      Lee Jay Lorenzen        */
/*      merge High C vers. w. 2.2 & 3.0         8/19/87         mdf     */
/*      add beep in chkkbd                      11/12/87        mdf     */

/*
*       Copyright 1999, Caldera Thin Clients, Inc.
*                 2002-2021 The EmuTOS development team
*
*       This software is licenced under the GNU Public License.
*       Please see LICENSE.TXT for further information.
*
*                  Historical Copyright
*       -------------------------------------------------------------
*       GEM Application Environment Services              Version 2.3
*       Serial No.  XXXX-0000-654321              All Rights Reserved
*       Copyright (C) 1987                      Digital Research Inc.
*       -------------------------------------------------------------
*/

/* #define ENABLE_KDEBUG */

#include "emutos.h"
#include "gemdisp.h"
#include "string.h"
#include "struct.h"
#include "aesvars.h"
#include "obdefs.h"

#include "geminput.h"
#include "gempd.h"
#include "extmsg.h"
#include "gemgsxif.h"
#include "gemaplib.h"
#include "geminit.h"
#include "gemflag.h"
#include "gemasm.h"
#include "optimize.h"
#include "gemdosif.h"
#include "gemqueue.h"
#include "gempd.h"
#include "extmsg.h"
#include "sched_abi.h"

#include "asm.h"

#define KEYMASK 0xffff0000L             /* for comparing data to KEYSTOP */
#define KEYSTOP 0x2b1c0000L             /* control-backslash */


/*
 * forkq(): put an FPD (containing a function address and a parameter) into the fork ring
 *
 * this is expected to be called with interrupts disabled
 *
 * returns -ve value iff it fails (the fork ring is full)
 */
WORD forkq(FCODE fcode, LONG fdata)
{
    FPD *f;

    if (fpcnt < NFORKS)
    {
        f = &D.g_fpdx[fpt++];
        if (fpt == NFORKS)      /* wrap pointer around  */
            fpt = 0;

        f->f_code = fcode;
        f->f_data = fdata;

        fpcnt++;
        return 0;   /* forkq() succeeded */
    }

    KDEBUG(("forkq() failed: fcode=%p, fdata=0x%08lx\n",fcode,fdata));
    return -1;      /* forkq() failed */
}


/*
 * forker(): remove all FPDs from the fork ring, calling the specified function each time
 *
 * this also handles event recording for the AES function appl_trecd()
 */
void forker(void)
{
    FPD *f;
    AESPD *oldrl;
    FPD g;

    oldrl = rlr;
    rlr = (AESPD *) -1;
    while(fpcnt)
    {
        /* critical area        */
        disable_interrupts();
        fpcnt--;
        f = &D.g_fpdx[fph++];

        /* copy FPD so an interrupt doesn't overwrite it */
        memcpy(&g, f, sizeof(FPD));
        if (fph == NFORKS)
            fph = 0;
        enable_interrupts();

        /* see if recording */
        if (gl_recd)
        {
            /* check for stop key */
            if ((g.f_code == kchange) && ((g.f_data&KEYMASK) == KEYSTOP))
                gl_recd = FALSE;

            /* if still recording, then handle event */
            if (gl_recd)
            {
                /* if it's a time event & the previously recorded one
                 * was also a time event, then coalesce them.
                 * otherwise record the event
                 */
                if ((g.f_code == tchange) && ((gl_rbuf-1)->f_code == tchange))
                {
                    (gl_rbuf-1)->f_data += g.f_data;
                }
                else
                {
                    memcpy(gl_rbuf, f, sizeof(FPD));
                    gl_rbuf++;
                    gl_rlen--;
                    if (gl_rlen <= 0)
                        gl_recd = FALSE;
                }
            }
        }

        (*g.f_code)(g.f_data);
    }

    rlr = oldrl;
}


void chkkbd(void)
{
    WORD achar, kstat;

    if (gl_play)
        return;

    kstat = gsx_kstate();
    achar = 0;

    /* only get a key if there's room in the buffer */
    if (gl_mowner->p_cda->c_q.c_cnt < KBD_SIZE)
        achar = gsx_char();     /* returns 0 if no key available */

    if (achar || (kstat != kstate))
    {
        disable_interrupts();
        forkq(kchange, MAKE_ULONG(achar, kstat));
        enable_interrupts();
    }
}


/*
 *  A device with something to say to an application fills this in; the
 *  AES asks, rather than the device reaching in.  It returns the process
 *  id to deliver to and fills a 16-byte message, or -1 when there is
 *  nothing.  Left null on machines where nothing does this.
 */
WORD (*aes_extmsg)(WORD *msg);

static void take_extmsg(void)
{
    WORD msg[8];
    WORD pid;

    if (aes_extmsg == NULL)
        return;

    while ((pid = (*aes_extmsg)(msg)) >= 0)
        msg_post(fpdnm(NULL, (UWORD)pid), msg);
}


/*
 * Everything that has piled up for the processes: the keyboard, messages
 * from devices, and the fork ring -- input, timers, whatever interrupts
 * queued.  Waking the processes it is for happens on the way, in
 * signal() (gemasync.c) through aes_wake().
 *
 * Not again from inside: a fork function may end up in dsptch().
 */
static BOOL woken;

static void pump(void)
{
    if (indisp)
        return;
    indisp = TRUE;
    chkkbd();
    take_extmsg();
    while (fpcnt)
        forker();
    indisp = FALSE;
}

/* p has what it waited for: let the kernel run it again */
void aes_wake(AESPD *p)
{
    p->p_stat &= ~WAITIN;
    k_wake(p->p_task);
    woken = TRUE;
}

/*
 * dsptch(): give the processor away.
 *
 * A process that has set WAITIN sleeps until aes_wake(), unless what it
 * waits for has already happened; any other stays runnable.  Which
 * process runs next is the kernel's decision, not ours.
 */
void dsptch(void)
{
    AESPD *p = rlr;

    if (indisp)         /* from a fork function: carry on */
        return;

    pump();

    if ((p->p_stat & WAITIN) && !(p->p_evwait & p->p_evflg))
        k_block();
    else
    {
        p->p_stat &= ~WAITIN;
        k_yield();
    }

    rlr = p;            /* back: whoever ran meanwhile set it to itself */
}

/*
 * The kernel calls this on core 0 when no process can run.  None is
 * running either, so rlr points at none while the input is handed out --
 * signal() must not take the process that went to sleep for the running
 * one.  Sleep until the next interrupt unless someone was woken.
 */
static void aes_idle(void)
{
    AESPD *p = rlr;

    woken = FALSE;
    rlr = NULL;
    pump();
    rlr = p;

#if USE_STOP_INSN_TO_FREE_HOST_CPU
    if (!woken && !fpcnt)
        stop_until_interrupt();
#endif
}

/* process 0 is the kernel task that runs the AES start-up */
void aes_sched_init(AESPD *p0)
{
    p0->p_task = k_current();
    k_set_idle(aes_idle);
}

/* the AES ends (shutdown, resolution change): its other processes too */
void aes_sched_exit(void)
{
    WORD i;

    for (i = 1; i < totpds; i++)
    {
        AESPD *p = pd_index(i);

        if (p->p_task)
        {
            k_task_kill(p->p_task);
            p->p_task = 0;
        }
    }
}
