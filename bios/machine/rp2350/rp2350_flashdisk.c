/*
 * rp2350_flashdisk.c - a drive in the QSPI flash, with wear levelling
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Flash can only be erased in blocks of 4 KB, and only whole blocks at a
 * time, while a file system rewrites the same few sectors (FAT, directory)
 * over and over.  Writing them in place would wear those blocks out and
 * lose everything in the block on a power failure.  So this driver never
 * overwrites a sector in place; it works like the translation layer inside
 * a USB stick:
 *
 *  - each 4 KB block holds a header page and seven 512-byte slots;
 *  - a written sector goes to the next free slot, and gets a tag in the
 *    header (its logical sector number and a sequence number);
 *  - the previous copy is then marked obsolete -- flash bits can always be
 *    programmed from 1 to 0, so this needs no erase;
 *  - when free blocks run short, the block with the fewest valid sectors
 *    is copied elsewhere and erased ("garbage collection");
 *  - a new block is taken from the least erased free block, and now and
 *    then a full block with a low erase count is moved out of the way, so
 *    that blocks holding data that never changes wear too.
 *
 * Power failures: a sector is written, then tagged, then the old copy is
 * marked obsolete.  A half-written sector has no tag and is ignored; if
 * the machine dies between tagging and marking, both copies are valid and
 * the one with the higher sequence number wins.  The file system is then
 * back to its state before that write, never inconsistent.
 */

#include "emutos.h"
#include "rp2350.h"
#include "rp2350_flash.h"
#include "rp2350_flashdisk.h"
#include "disk.h"
#include "gemerror.h"
#include "string.h"
#include "blkdev.h"
#include "kprint.h"

#if CONF_WITH_RP2350_FLASHDISK

/* the flash behind the real-time core image, 7 MB */
#define FD_FLASH_OFFSET 0x00900000UL
#define FD_FLASH_SIZE   0x00700000UL
#define FD_XIP_BASE     (0x10000000UL + FD_FLASH_OFFSET)

#define BLOCK_SIZE      4096
#define FD_BLOCKS       (FD_FLASH_SIZE / BLOCK_SIZE)    /* 1792 */
#define SLOTS           7                               /* per block */
#define HEADER_SIZE     256     /* SECTOR_SIZE (512) comes from disk.h */

/* 4 MB of it are the drive; the rest is spare, which keeps garbage
 * collection cheap and spreads the wear */
#define FD_SECTORS      8192UL

#define FD_MAGIC        0x44465450UL    /* "PTFD" */

#define TAG_FREE        0xffff
#define TAG_VALID       0xfffe          /* bit 0 programmed to 0 */
#define TAG_OBSOLETE    0xfffc          /* bit 1 as well */

#define NO_SLOT         0xffff
#define GC_RESERVE      3               /* free blocks kept in hand */
#define STATIC_WL_EVERY 64              /* GCs between wear levelling moves */

struct tag
{
    UWORD lsn;
    UWORD flags;
    ULONG seq;
};

struct header
{
    ULONG magic;
    ULONG erases;
    ULONG reserved[2];
    struct tag tag[SLOTS];
};

static UWORD map[FD_SECTORS];       /* logical sector -> slot, or NO_SLOT */
static UBYTE used[FD_BLOCKS];       /* slots written in the block */
static UBYTE valid[FD_BLOCKS];      /* of those, still current */
static ULONG next_seq;
static WORD open_block;             /* block being filled, or -1 */
static ULONG gc_count;
static BOOL collecting;
static BOOL mounted;

/* blocks that hold nothing and can be taken next */
static UWORD count_free(void)
{
    UWORD n = 0;
    WORD i;

    for (i = 0; i < FD_BLOCKS; i++)
        if (used[i] == 0 && i != open_block)
            n++;
    return n;
}

static const struct header *hdr(WORD block)
{
    return (const struct header *)(FD_XIP_BASE + (ULONG)block * BLOCK_SIZE);
}

static const UBYTE *slot_data(UWORD slot)
{
    WORD block = slot / SLOTS;
    WORD i = slot % SLOTS;

    return (const UBYTE *)(FD_XIP_BASE + (ULONG)block * BLOCK_SIZE
                           + HEADER_SIZE * 2 + (ULONG)i * SECTOR_SIZE);
}

/* program one header page: everything 0xff except what is given */
static void program_header(WORD block, const struct header *h)
{
    static UBYTE page[HEADER_SIZE];

    memset(page, 0xff, sizeof(page));
    memcpy(page, h, sizeof(*h));
    rp2350_flash_program(FD_FLASH_OFFSET + (ULONG)block * BLOCK_SIZE,
                         page, HEADER_SIZE);
}

static void write_magic(WORD block, ULONG erases)
{
    struct header h;

    memset(&h, 0xff, sizeof(h));
    h.magic = FD_MAGIC;
    h.erases = erases;
    program_header(block, &h);
}

static void write_tag(WORD block, WORD i, UWORD lsn, UWORD flags, ULONG seq)
{
    struct header h;

    memset(&h, 0xff, sizeof(h));
    h.tag[i].lsn = lsn;
    h.tag[i].flags = flags;
    h.tag[i].seq = seq;
    program_header(block, &h);
}

static void obsolete_slot(UWORD slot)
{
    WORD block = slot / SLOTS;
    WORD i = slot % SLOTS;
    struct header h;

    memset(&h, 0xff, sizeof(h));
    h.tag[i].flags = TAG_OBSOLETE;      /* only clears bits */
    program_header(block, &h);
    if (valid[block])
        valid[block]--;
}

static void erase_block(WORD block)
{
    ULONG erases = hdr(block)->magic == FD_MAGIC ? hdr(block)->erases + 1 : 1;

    rp2350_flash_erase(FD_FLASH_OFFSET + (ULONG)block * BLOCK_SIZE, BLOCK_SIZE);
    write_magic(block, erases);
    used[block] = 0;
    valid[block] = 0;
}

/*
 * Pick the next block to fill: the free block with the lowest erase count.
 */
static WORD pick_free_block(void)
{
    WORD block, best = -1;
    ULONG best_erases = 0xffffffffUL;
    WORD i;

    for (i = 0; i < FD_BLOCKS; i++)
    {
        block = i;
        if (used[block] || block == open_block)
            continue;
        if (hdr(block)->magic != FD_MAGIC)   /* erased, count unknown */
            return block;
        if (hdr(block)->erases < best_erases)
        {
            best_erases = hdr(block)->erases;
            best = block;
        }
    }
    return best;
}

static void copy_out(WORD block);   /* forward, used by garbage collection */

/*
 * Free a block: the one with the fewest valid sectors, so that as little
 * as possible has to be copied.  Every so often take a block with a low
 * erase count instead, even when it is full of valid data: without that,
 * blocks holding files that are never rewritten would never wear at all
 * while the rest of the flash does.
 */
static void collect(void)
{
    WORD i, victim = -1;
    WORD best_valid = SLOTS + 1;
    ULONG best_erases = 0xffffffffUL;
    BOOL wear_move = (++gc_count % STATIC_WL_EVERY) == 0;

    if (collecting)                     /* copy_out() writes; never recurse */
        return;
    collecting = TRUE;

    for (i = 0; i < FD_BLOCKS; i++)
    {
        if (used[i] == 0 || i == open_block)
            continue;
        if (wear_move)
        {
            if (hdr(i)->magic == FD_MAGIC && hdr(i)->erases < best_erases)
            {
                best_erases = hdr(i)->erases;
                victim = i;
            }
        }
        else if (valid[i] < best_valid)
        {
            best_valid = valid[i];
            victim = i;
        }
    }
    if (victim >= 0)
    {
        copy_out(victim);
        erase_block(victim);
    }
    collecting = FALSE;
}

static LONG write_sector(ULONG lsn, const UBYTE *buf);

/* move every valid sector of a block elsewhere */
static void copy_out(WORD block)
{
    const struct header *h = hdr(block);
    WORD i;

    for (i = 0; i < SLOTS; i++)
    {
        UWORD slot = block * SLOTS + i;

        if (h->tag[i].flags != TAG_VALID || h->tag[i].lsn >= FD_SECTORS)
            continue;
        if (map[h->tag[i].lsn] != slot)  /* already superseded */
            continue;
        write_sector(h->tag[i].lsn, slot_data(slot));
    }
}

static LONG write_sector(ULONG lsn, const UBYTE *buf)
{
    UWORD old = map[lsn];
    WORD block, i;
    UWORD slot;

    if (open_block < 0 || used[open_block] >= SLOTS)
    {
        if (count_free() <= GC_RESERVE)
            collect();
        block = pick_free_block();
        if (block < 0)
            return EWRPRO;              /* full beyond repair */
        if (hdr(block)->magic != FD_MAGIC)
            write_magic(block, 1);
        open_block = block;
    }
    block = open_block;
    i = used[block];
    slot = block * SLOTS + i;

    /* data first, then the tag: a sector without a tag is ignored */
    rp2350_flash_program(FD_FLASH_OFFSET + (ULONG)block * BLOCK_SIZE
                         + HEADER_SIZE * 2 + (ULONG)i * SECTOR_SIZE,
                         buf, SECTOR_SIZE);
    write_tag(block, i, (UWORD)lsn, TAG_VALID, next_seq++);
    used[block]++;
    valid[block]++;
    map[lsn] = slot;

    if (old != NO_SLOT)
        obsolete_slot(old);

    return E_OK;
}

/* ---- mounting ---- */

static void scan(void)
{
    WORD block, i;

    memset(map, 0xff, sizeof(map));
    memset(used, 0, sizeof(used));
    memset(valid, 0, sizeof(valid));
    next_seq = 1;

    for (block = 0; block < FD_BLOCKS; block++)
    {
        const struct header *h = hdr(block);

        if (h->magic != FD_MAGIC)
            continue;                   /* erased, or not ours: free */
        for (i = 0; i < SLOTS; i++)
        {
            UWORD slot = block * SLOTS + i;
            UWORD lsn = h->tag[i].lsn;

            if (h->tag[i].flags == TAG_FREE)
                continue;               /* not written (yet) */
            used[block] = i + 1;
            if (h->tag[i].flags != TAG_VALID || lsn >= FD_SECTORS)
                continue;               /* obsolete or nonsense */
            if (h->tag[i].seq >= next_seq)
                next_seq = h->tag[i].seq + 1;
            if (map[lsn] == NO_SLOT)
            {
                map[lsn] = slot;
                valid[block]++;
            }
            else
            {
                /* two copies: the newer one wins (a power failure between
                 * writing the tag and marking the old copy obsolete) */
                WORD other = map[lsn] / SLOTS;
                const struct tag *t = &hdr(other)->tag[map[lsn] % SLOTS];

                if (h->tag[i].seq > t->seq)
                {
                    valid[other]--;
                    map[lsn] = slot;
                    valid[block]++;
                }
            }
        }
    }
    open_block = -1;    /* never append to a block written before the scan */
}

/*
 * A freshly erased flash gets a FAT16 file system: boot sector and the
 * first sector of each FAT.  Everything else reads back as zeros, which
 * is what an empty directory and empty FAT entries look like.
 */
#define FD_RESERVED     1
#define FD_FATS         2
#define FD_FATSECS      32
#define FD_ROOTENTS     256

static void put16(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)v;
    p[1] = (UBYTE)(v >> 8);
}

static void format(void)
{
    UBYTE sec[SECTOR_SIZE];

    memset(sec, 0, sizeof(sec));
    sec[0] = 0xeb;                      /* jump, as DOS writes it */
    sec[1] = 0x3c;
    sec[2] = 0x90;
    memcpy(sec + 3, "PTOS3000", 8);
    put16(sec + 11, SECTOR_SIZE);       /* bytes per sector */
    sec[13] = 1;                        /* sectors per cluster */
    put16(sec + 14, FD_RESERVED);
    sec[16] = FD_FATS;
    put16(sec + 17, FD_ROOTENTS);
    put16(sec + 19, (UWORD)FD_SECTORS);
    sec[21] = 0xf8;                     /* fixed disk */
    put16(sec + 22, FD_FATSECS);
    put16(sec + 24, 32);                /* sectors per track (unused) */
    put16(sec + 26, 2);                 /* heads (unused) */
    sec[38] = 0x29;                     /* extended boot signature */
    memcpy(sec + 43, "PTOS FLASH ", 11);
    memcpy(sec + 54, "FAT16   ", 8);
    sec[510] = 0x55;
    sec[511] = 0xaa;
    write_sector(0, sec);

    memset(sec, 0, sizeof(sec));
    sec[0] = 0xf8;                      /* media byte, then end of chain */
    sec[1] = 0xff;
    sec[2] = 0xff;
    sec[3] = 0xff;
    write_sector(FD_RESERVED, sec);
    write_sector(FD_RESERVED + FD_FATSECS, sec);

    KINFO(("flashdisk: formatted, %ld sectors\n", (LONG)FD_SECTORS));
}

void rp2350_flashdisk_init(void)
{
    if (!rp2350_flash_init())
    {
        KINFO(("flashdisk: no bootrom flash functions\n"));
        return;
    }
    scan();
    mounted = TRUE;
    if (map[0] == NO_SLOT)              /* nothing on it yet */
        format();
    KINFO(("flashdisk: %u blocks, %u free, next sequence %ld\n",
           (UWORD)FD_BLOCKS, count_free(), (LONG)next_seq));
}

/* ---- the block device ---- */

LONG rp2350_flashdisk_ioctl(UWORD dev, UWORD ctrl, void *arg)
{
    ULONG *info = (ULONG *)arg;

    if (dev != 0 || !mounted)
        return EUNDEV;

    switch(ctrl)
    {
    case GET_DISKINFO:
        info[0] = FD_SECTORS;
        info[1] = SECTOR_SIZE;
        return E_OK;
    case GET_DISKNAME:
        strcpy((char *)arg, "Internal flash");
        return E_OK;
    case GET_MEDIACHANGE:
        return MEDIANOCHANGE;
    }

    return ERR;
}

LONG rp2350_flashdisk_rw(WORD rw, LONG sector, WORD count, UBYTE *buf, WORD dev)
{
    if (dev != 0 || !mounted)
        return EUNDEV;
    if (sector < 0 || (ULONG)sector + count > FD_SECTORS)
        return ESECNF;

    for ( ; count > 0; count--, sector++, buf += SECTOR_SIZE)
    {
        if (rw & RW_WRITE)
        {
            LONG ret = write_sector(sector, buf);

            if (ret != E_OK)
                return ret;
        }
        else if (map[sector] == NO_SLOT)
            memset(buf, 0, SECTOR_SIZE);    /* never written */
        else
            memcpy(buf, slot_data(map[sector]), SECTOR_SIZE);
    }

    return E_OK;
}

#endif /* CONF_WITH_RP2350_FLASHDISK */
