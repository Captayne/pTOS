/*
 * super.c - GEMDOS Super() for ARMv8-M (Cortex-M33)
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Called from bdos/arch/armv8m/rwa.S with the caller's resume record (see
 * bios/arch/armv8m/vectorsasm.S).  Switching mode only means editing that
 * record: the caller comes back through it with the new CONTROL and stack
 * selection.
 *
 * m68k user mode maps to unprivileged Thread mode on the process stack
 * (PSP), supervisor mode to privileged Thread mode.  Super() from user
 * mode keeps running on the caller's stack, like Super(0L) on m68k (and
 * like the A-profile port, which uses the ARM "system" mode for the same
 * purpose): interrupts always use the main stack on M-profile, so there is
 * no need to switch stacks just to be in supervisor mode.
 */

#include "config.h"
#include "portab.h"
#include "string.h"

#define REC_FRAME       0       /* caller's exception frame */
#define REC_OTHER_SP    1       /* the stack pointer not in use by the caller */
#define REC_CONTROL     2       /* bit 0: unprivileged; PRIMASK in bit 31 */
#define REC_EXC_RETURN  3       /* bit 2: caller's frame is on the PSP */

#define CONTROL_NPRIV   1UL
#define EXC_RETURN_PSP  4UL
#define XPSR_PADDED     0x200UL

LONG armv8m_super(ULONG *rec, LONG arg);

LONG armv8m_super(ULONG *rec, LONG arg)
{
    BOOL user = (rec[REC_CONTROL] & CONTROL_NPRIV) != 0;
    LONG old_ssp;

    if (arg == 1L)                          /* Super(1L): inquire */
        return user ? 0L : -1L;

    if (user)
    {
        /* to supervisor mode, on the same stack; the "old SSP" handed back
         * is the main stack pointer, which is what the caller would pass
         * back to return to user mode */
        rec[REC_CONTROL] &= ~CONTROL_NPRIV;
        return (LONG)rec[REC_OTHER_SP];
    }

    /* back to user mode */
    old_ssp = 0L;
    if (!(rec[REC_EXC_RETURN] & EXC_RETURN_PSP))
    {
        /* The caller has been running on the main stack: move its frame
         * over to the process stack, and make the main stack what it was
         * below the frame. */
        ULONG *frame = (ULONG *)rec[REC_FRAME];
        ULONG *uframe = (ULONG *)rec[REC_OTHER_SP] - 8;

        memcpy(uframe, frame, 8 * sizeof(ULONG));
        uframe[7] &= ~XPSR_PADDED;
        rec[REC_OTHER_SP] = (ULONG)(frame + 8 + ((frame[7] & XPSR_PADDED) ? 1 : 0));
        rec[REC_FRAME] = (ULONG)uframe;
        rec[REC_EXC_RETURN] |= EXC_RETURN_PSP;
    }
    else if (arg)
    {
        /* Super(ssp) after Super(0L): ssp is the main stack pointer to use */
        rec[REC_OTHER_SP] = (ULONG)arg;
    }
    rec[REC_CONTROL] |= CONTROL_NPRIV;

    return old_ssp;
}
