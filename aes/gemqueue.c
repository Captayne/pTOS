/*      GEMQUEUE.C      1/27/84 - 07/09/85      Lee Jay Lorenzen        */
/*      merge High C vers. w. 2.2               8/21/87         mdf     */

/*
*       Copyright 1999, Caldera Thin Clients, Inc.
*                 2002-2024 The EmuTOS development team
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

#include "emutos.h"
#include "string.h"
#include "struct.h"
#include "obdefs.h"
#include "aesdefs.h"

#include "rectfunc.h"
#include "gemasync.h"
#include "gemqueue.h"



static void doq(WORD donq, AESPD *p, QPB *m)
{
    WORD n, index;
    WORD *om, *nm;

    n = m->qpb_cnt;
    if (donq)
    {
        memcpy(p->p_qaddr+p->p_qindex, (char *)m->qpb_buf, n);
        /*
         * if it's a redraw msg, try to find a matching msg and
         * union the redraw rectangles together
         */
        nm = (WORD *) &p->p_queue[p->p_qindex];
        if ((nm[0] == WM_REDRAW) || (nm[0] == WM_ARROWED) || (nm[0] == WM_HSLID) || (nm[0] == WM_VSLID))
        {
            index = 0;
            while ((index < p->p_qindex) && n)
            {
                om = (WORD *) &p->p_queue[index];
                /* if redraw and same handle then union */
                if ((om[0] == WM_REDRAW) && (nm[3] == om[3]))
                {
                    rc_union((GRECT *)&nm[4], (GRECT *)&om[4]);  /* FIXME: Ugly pointer typecasting */
                    n = 0;
                }
                else
                {
                    /*
                     * if another arrow command then copy over with new msg
                     * else add in length and get next msg
                     */
                    if (om[0] == WM_ARROWED)
                    {
                        memcpy(om, nm, 16);
                        n = 0;
                    }
                    else
                        index += ((UWORD)om[2] != 0xFFFF) ? (om[2] + 16) : 16;
                }
            }
        }
        p->p_qindex += n;
    }
    else
    {
        memcpy((char *)m->qpb_buf, p->p_qaddr, n);
        p->p_qindex -= n;
        if (p->p_qindex)
            memcpy(p->p_qaddr, p->p_qaddr+n, p->p_qindex);
    }
}


/*
 *  Put a message into a process' pipe without waiting.
 *
 *  ap_rdwr() blocks its caller when the pipe is full, which is right for
 *  an application and impossible for anyone running in the scheduler's
 *  own context -- there is no caller there to block.  This one refuses
 *  instead, and whoever asked has to make do with that: the sender
 *  coalesces, so nothing is lost but an intermediate value.
 *
 *  Returns 1 when the message was taken, 0 when there was no room.
 */
WORD msg_post(AESPD *p, const WORD *msg)
{
    EVB *e;

    if (p == NULL || (QUEUE_SIZE - p->p_qindex) < 16)
        return 0;

    memcpy(p->p_qaddr + p->p_qindex, msg, 16);
    p->p_qindex += 16;

    /* Hand it straight to a reader that is already waiting, exactly as
       aqueue() does; otherwise it stays in the pipe until one asks. */
    if ((e = p->p_qdq) != 0)
    {
        e->e_flag |= NOCANCEL;
        p->p_qdq = e->e_link;
        if (e->e_link)
            e->e_link->e_pred = e->e_pred;
        doq(0, p, (QPB *)e->e_parm);
        azombie(e, 1);
    }
    return 1;
}


void aqueue(WORD isqwrite, EVB *e, LONG lm)
{
    AESPD   *p;
    QPB     *m;
    EVB     **ppe;
    WORD    qready;

    m = (QPB *)lm;

    p = m->qpb_ppd;

    if (isqwrite)
        qready = (m->qpb_cnt <= (QUEUE_SIZE-p->p_qindex));
    else
        qready = (p->p_qindex > 0);

    ppe = (isqwrite ^ qready) ? &p->p_qnq : &p->p_qdq;

    /* room for message or messages in q */
    if (qready)
    {
        doq(isqwrite, p, m);
        azombie(e, 1);          /* ap_rdwr() will return 1 => OK */ 
        if ((e = *ppe) != 0)    /* assignment ok */
        {
            e->e_flag |= NOCANCEL;
            *ppe = e->e_link;

            if (e->e_link)
                e->e_link->e_pred = e->e_pred;

            doq(!isqwrite, p, (QPB *)e->e_parm);
            azombie(e, 1);
        }
    }
    else            /* "block" the event */
    {
        e->e_parm = lm;
        evinsert(e, ppe);
    }
}
