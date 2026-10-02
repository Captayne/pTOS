/*
 * fsbuf.c - buffer mgmt for file system
 *
 * Copyright (C) 2001 Lineo, Inc.
 *               2002-2022 The EmuTOS development team
 *
 * Authors:
 *  SCC   Steve C. Cavender
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/* #define ENABLE_KDEBUG */

#include "emutos.h"
#include "fs.h"
#include "gemerror.h"
#include "biosbind.h"
#include "ahdi.h"
#include "mem.h"
#include "string.h"
#include "biosext.h"
#include "kprint.h"

#ifdef __arm__
BCB *bufl[2];           /* buffer lists - two lists:  FAT and dir/data --
                         * fixed address on m68k (tosvars.ld), ordinary
                         * storage here (#219) */
#else
extern BCB *bufl[];     /* buffer lists - two lists:  FAT and dir/data */
#endif

#define NUMBUFS 2       /* buffers per list */

/* creates a chain of BCBs and corresponding buffers */
static void *create_chain(UBYTE *p,LONG n)
{
    BCB *bcbptr;
    WORD i;

    for (i = 0; i < NUMBUFS; i++, p += n) {
        bcbptr = (BCB *)p;
        bzero(bcbptr,sizeof(BCB));
        if (i < NUMBUFS-1)                  /* chain to next */
            bcbptr->b_link = (BCB *)(p + n);
        bcbptr->b_bufdrv = -1;              /* mark as invalid */
        bcbptr->b_bufr = (char *)p + sizeof(BCB);
    }

    return p;
}

/*
 * bufl_init - BDOS buffer list initialization
 *
 * this must be called before memory is initialised, because we use
 * balloc_stram().  we use balloc_stram() because we must not use
 * Malloc() until after we have finished booting.  this is because TOS
 * doesn't, and some programs that are direct-booted from a disk may
 * therefore assume that all memory from membot upwards is available
 * (I'm looking at you, Dungeon Master).
 */
void bufl_init(void)
{
    UBYTE *p;
    LONG n;

    n = sizeof(BCB) + pun_ptr->max_sect_siz;
    p = balloc_stram(2L*NUMBUFS*n, FALSE);
    if (!p)
        panic("bufl_init(%ld): no memory\n",2L*NUMBUFS*n);

    /* set up FAT chain */
    bufl[BI_FAT] = (BCB *)p;
    p = create_chain(p,n);

    /* set up dir/data chain */
    bufl[BI_DATA] = (BCB *)p;
    create_chain(p,n);
}



/*
 * flush -
 *
 * NOTE: longjmp_rwabs() is a macro that includes a longjmp() which is
 *       executed if the BIOS returns an error, therefore flush() does
 *       not need to return any error codes.
 */

void flush(BCB *b)
{
    int n,d;
    DMD *dm;

    dm = b->b_dm;               /*  media descr for buffer      */
    n = b->b_buftyp;
    d = b->b_bufdrv;
    b->b_bufdrv = -1;           /* invalidate in case of error */

    longjmp_rwabs(1, (long)b->b_bufr, 1, b->b_bufrec+dm->m_recoff[n], d);

    /* flush to both fats */

    if (n == BT_FAT && !dm->m_1fat) {
        longjmp_rwabs(1, (long)b->b_bufr, 1,
                      b->b_bufrec+dm->m_recoff[BT_FAT]-dm->m_fsiz, d);
    }
    b->b_bufdrv = d;                    /* re-validate */
    b->b_dirty = 0;
}


/*
 *  flush_all_buffers - write every dirty buffer out, now
 *
 *  For a medium that is about to leave.  Lending the SD card to the other
 *  machine over USB (bios/disk.c's disk_lend_sd()) reports the drive as
 *  changed, and GEMDOS then throws its buffers away -- which is right for
 *  a floppy somebody swapped, and wrong here, because some of those
 *  buffers had been written to and had not gone out yet.  Thrown away
 *  means thrown away: the FAT sector or the directory sector never
 *  arrives, the other machine writes its own version over the top, and
 *  what comes back is a directory that disagrees with the FAT.  That is
 *  what cost a card its contents twice in one day.
 *
 *  Not flush() above, although the work is the same.  flush() reports a
 *  BIOS error by longjmp()ing into the error buffer a GEMDOS call set up
 *  for itself, and this runs from the BIOS, outside any such call: the
 *  jump would land in a frame that is no longer there.  So the write goes
 *  out through plain rwabs(), and a buffer that could not be written
 *  keeps its dirty flag rather than pretending to be clean.
 *
 *  Returns E_OK, or the first error, having tried all of them regardless:
 *  one unwritable buffer is no reason to abandon the others.
 */
/* Rwabs the way longjmp_rwabs() calls it, but returning the error
   instead of jumping: past 32767 the record number goes in the long
   argument and the short one is -1. */
static LONG rwabs_rec(void *buf, LONG rec, int dev)
{
    if (rec <= 32767L)
        return Rwabs(1, (long)buf, 1, (int)rec, dev, 0);

    return Rwabs(1, (long)buf, 1, -1, dev, rec);
}

LONG flush_all_buffers(void)
{
    LONG err = E_OK;
    BCB *b;
    int i, n, d;
    DMD *dm;

    for (i = 0; i < 2; i++)
    {
        for (b = bufl[i]; b; b = b->b_link)
        {
            LONG ret;

            if ((b->b_bufdrv == -1) || !b->b_dirty)
                continue;

            dm = b->b_dm;
            n = b->b_buftyp;
            d = b->b_bufdrv;

            ret = rwabs_rec(b->b_bufr, b->b_bufrec + dm->m_recoff[n], d);

            /* the second FAT, where there is one */
            if ((ret == E_OK) && (n == BT_FAT) && !dm->m_1fat)
                ret = rwabs_rec(b->b_bufr,
                                b->b_bufrec + dm->m_recoff[BT_FAT] - dm->m_fsiz,
                                d);

            if (ret == E_OK)
                b->b_dirty = 0;
            else if (err == E_OK)
                err = ret;
        }
    }

    return err;
}



/*
 * getbcb - called by getrec() to get the BCB for the desired record
 *
 * buftype is BT_FAT, BT_ROOT, or BT_DATA
 */
BCB *getbcb(DMD *dmd,WORD buftype,RECNO recnum)
{
    BCB *b;
    BCB *p, *mtbuf, **q, **phdr;
    int err;

    mtbuf = 0;
    phdr = &bufl[buftype==BT_FAT ? BI_FAT : BI_DATA];

    /*
     * See if the desired record for the desired drive is in memory.
     * If it is, we will use it.  Otherwise we will use
     *          the last invalid (available) buffer,  or
     *          the last (least recently) used buffer.
     */

    for (b = *(q = phdr); b; b = *(q = (BCB **)(void *)b))
    {
        if ((b->b_bufdrv == dmd->m_drvnum) && (b->b_buftyp == buftype) && (b->b_bufrec == recnum))
            break;
        /*
         * keep track of the last invalid buffer
         */
        if (b->b_bufdrv == -1)  /*  if buffer not valid */
            mtbuf = b;          /*    then it's 'empty' */
    }

    if (!b)
    {
        /*
         * not in memory.  If there was an 'empty' buffer, use it.
         */
        if (mtbuf)
            b = mtbuf;

        /*
         * find predecessor of mtbuf, or last guy in list, which
         * is the least recently used.
         */

doio:   for (p = *(q = phdr); p->b_link; p = *(q = (BCB **)(void *)p))
            if (b == p)
                break;
        b = p;

        /*
         * if the buffer is dirty, flush it, then read in the new record
         */
        if ((b->b_bufdrv != -1) && b->b_dirty)
            flush(b);
        b->b_bufdrv = -1;       /* in case longjmp_rwabs() fails */
        longjmp_rwabs(0, (long)b->b_bufr, 1, recnum+dmd->m_recoff[buftype], dmd->m_drvnum);

        /*
         * make the new buffer current
         */

        b->b_bufrec = recnum;
        b->b_dirty = 0;
        b->b_buftyp = buftype;
        b->b_bufdrv = dmd->m_drvnum;
        b->b_dm = dmd;
    }
    else
    {   /* use a buffer, but first validate media */
        err = Mediach(b->b_bufdrv);
        if (err != 0) {
            if (err == 1) {
                goto doio; /* media may be changed */
            } else if (err == 2) {
                /* media definitely changed */
                errdrv = b->b_bufdrv;
                rwerr = E_CHNG; /* media change */
                errcode = rwerr;
                longjmp(errbuf,1);
            }
        }
    }

    /*
     *  now put the current buffer at the head of the list
     */

    *q = b->b_link;
    b->b_link = *phdr;
    *phdr = b;

    return b;
}



/*
 * getrec - return the ptr to the buffer containing the desired record
 */
UBYTE *getrec(RECNO recn, OFD *of, int wrtflg)
{
    DMD *dm = of->o_dmd;
    BCB *b;
    int n;

    KDEBUG(("getrec 0x%lx, %p, 0x%x\n",recn,dm,wrtflg));

    /* put bcb management here */
    if (of->o_dmd->m_fatofd == of)  /* is this the OFD for the 'FAT file'? */
        n = BT_FAT;                 /* yes, must be FAT access             */
    else if (!of->o_dnode)          /* no - do we have a dir node?         */
        n = BT_ROOT;                /* no, must be root                    */
    else n = BT_DATA;               /* yes, must be normal dir/file        */

    KDEBUG(("n=%i, dm->m_recoff[n]=0x%lx\n",n,dm->m_recoff[n]));

    b = getbcb(dm,n,recn);          /* get BCB for buffer */

    /*
     * if we are writing to the buffer, dirty it
     */
    if (wrtflg)
        b->b_dirty = 1;

    return (UBYTE *)b->b_bufr;
}
