/*
 * rp2350_sched.c - sched_abi.h on the kernel pTOS runs on
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Every call goes straight through the kernel's call table, which
 * rp2350_rtx_init() found before the AES starts.  There is no scheduler
 * in pTOS to fall back on: without the kernel the AES cannot run, and
 * says so.
 */

#include "emutos.h"
#include "biosext.h"
#include "sched_abi.h"
#include "rtx_rp2350.h"
#include "rp2350_rtx.h"

/* every task starts with the same share of the processor */
#define DEFAULT_PRIO    1

static const struct kernel_api *kernel(void)
{
    if (rp2350_kernel == NULL)
        panic("no kernel to schedule the AES\n");
    return rp2350_kernel;
}

k_task_t k_task_create(void (*entry)(void), void *stack, unsigned long size)
{
    return kernel()->task_create(entry, stack, size, DEFAULT_PRIO);
}

void k_task_kill(k_task_t task)
{
    kernel()->task_kill(task);
}

void k_yield(void)
{
    kernel()->yield();
}

void k_block(void)
{
    kernel()->block();
}

void k_wake(k_task_t task)
{
    kernel()->wake(task);
}

k_task_t k_current(void)
{
    return kernel()->self();
}

void k_set_idle(void (*idle)(void))
{
    kernel()->set_idle(idle);
}
