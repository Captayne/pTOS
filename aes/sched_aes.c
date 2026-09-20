/*
 * sched_aes.c - the AES dispatcher behind the scheduler interface
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This software is licenced under the GNU Public License.
 * Please see LICENSE.TXT for further information.
 *
 * The scheduler pTOS has always had, reached through sched_abi.h.  It is
 * the default and it is complete: nothing else is needed to run pTOS,
 * and nothing here depends on anything outside the GPL tree.
 *
 * The policy is round robin.  insert_process() appends to the end of the
 * ready list and disp() takes the head, so tasks take turns in the order
 * they gave way; there are no priorities and no time slices.  The weight
 * of gemdisp.c is elsewhere -- the fork ring, the keyboard poll and the
 * idle path -- and all of that stays where it is.  This file only puts a
 * name on the two things the rest of the AES actually asks for.
 */

#include "emutos.h"
#include "sched_abi.h"

#include "struct.h"
#include "aesvars.h"
#include "gemasm.h"


void k_yield(void)
{
    dsptch();
}


void k_block(unsigned long reason)
{
    /* The AES has one wait state.  The reason is kept in the interface
       for implementations that tell waits apart; here it would only be
       written and never read. */
    (void)reason;

    rlr->p_stat |= WAITIN;
    dsptch();
}


k_task_t k_current(void)
{
    /* The running process is the head of the ready list.  Its address
       serves as the identity: opaque to the caller, unique while the
       process lives, and free. */
    return (k_task_t)(unsigned long)rlr;
}
