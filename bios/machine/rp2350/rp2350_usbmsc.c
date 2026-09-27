/*
 * rp2350_usbmsc.c - the SD card as a USB disk, when a program shares it
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This software is licenced under the GNU Public License.
 * Please see LICENSE.TXT for further information.
 *
 * The device already offers a serial console on its one USB port; this
 * adds a second function beside it, a disk, so that the SD card can be
 * opened in a file manager on the other machine.
 *
 * The whole card, not one of its partitions: the other machine then sees
 * the partition table too and mounts everything on it, which is what
 * somebody who plugs a cable in wants.  It used to be the drive in the
 * flash, which is gone -- and which was the wrong thing to share twice
 * over, because writing it meant erasing flash from inside this file's
 * interrupt handler, 23 milliseconds at a time with nothing else served.
 * A card write is an SPI transfer and takes none of that.
 *
 * THE MEDIUM IS ABSENT UNTIL A PROGRAM SAYS OTHERWISE
 *
 * The interface is always there, and until it is shared every command
 * that needs the medium is answered "not ready, medium not present" --
 * which is how a card reader with no card behaves, and which every host
 * understands.  The alternative, adding the interface only on demand,
 * would mean re-enumerating, and that would drop the console the deploy
 * terminal talks through.  A flag costs nothing and drops nothing.
 *
 * WHILE IT IS SHARED, IT IS NOT OURS
 *
 * Two file systems with their own caches on one medium corrupt it, and
 * not eventually -- reliably.  So sharing is exclusive: pTOS reports
 * the drive as changed and refuses it for as long as the other side
 * has it, and reports it changed again when it comes back, which is
 * what makes GEMDOS throw its buffers away.  That is the mechanism TOS
 * has always had for swapping a floppy, and it fits exactly.
 *
 * Bulk-Only Transport, and enough SCSI for a host to believe it: a
 * command wrapper of 31 bytes, an optional data phase, a status wrapper
 * of 13.  USB full speed gives about 1 MB/s at best, which is ample for
 * a 4 MB drive.
 */

#include "emutos.h"
#include "rp2350.h"
#include "rp2350_usb.h"
#include "rp2350_usbcon.h"
#include "sd.h"
#include "disk.h"
#include "biosdefs.h"
#include "disk.h"
#include "gemerror.h"
#include "string.h"
#include "biosext.h"
#include "cookie.h"
#include "usbdrv.h"

#if CONF_WITH_RP2350_USBMSC

/* ==== the wrappers ===================================================== */

#define CBW_SIGNATURE   0x43425355UL    /* "USBC", little endian */
#define CSW_SIGNATURE   0x53425355UL    /* "USBS" */
#define CBW_LEN         31
#define CSW_LEN         13

#define CSW_PASSED      0
#define CSW_FAILED      1
#define CSW_PHASE_ERROR 2

/* ==== the commands we answer =========================================== */

#define SCSI_TEST_UNIT_READY        0x00
#define SCSI_REQUEST_SENSE          0x03
#define SCSI_INQUIRY                0x12
#define SCSI_MODE_SENSE_6           0x1a
#define SCSI_START_STOP_UNIT        0x1b
#define SCSI_PREVENT_ALLOW_REMOVAL  0x1e
#define SCSI_READ_FORMAT_CAPACITIES 0x23
#define SCSI_READ_CAPACITY_10       0x25
#define SCSI_READ_10                0x28
#define SCSI_WRITE_10               0x2a
/*
 * The ones a host sends to a disk that is not removable.  Described as
 * removable, this driver was never asked for them; described as fixed --
 * which is what makes a host read the partition table as a partition
 * table -- it is, and the default branch below refused them all as
 * "invalid command".  A refused command never reaches the card, so it
 * raised the command count and not the read count.
 */
#define SCSI_VERIFY_10              0x2f
#define SCSI_SYNCHRONIZE_CACHE_10   0x35
#define SCSI_MODE_SENSE_10          0x5a
#define SCSI_SERVICE_ACTION_IN_16   0x9e
#define SAI_READ_CAPACITY_16        0x10    /* in the low 5 bits of cdb[1] */

/* sense keys, for REQUEST SENSE */
#define SENSE_NONE              0x00
#define SENSE_NOT_READY         0x02
#define SENSE_ILLEGAL_REQUEST   0x05
#define SENSE_UNIT_ATTENTION    0x06

#define ASC_MEDIUM_NOT_PRESENT      0x3a
#define ASC_INVALID_COMMAND         0x20
#define ASC_INVALID_FIELD_IN_CDB    0x24
#define ASC_MEDIUM_CHANGED          0x28

/* ==== state ============================================================ */

/* Where we are in the three phases.  A command that needs no data goes
   straight from COMMAND to STATUS. */
enum { PHASE_COMMAND, PHASE_DATA_IN, PHASE_DATA_OUT, PHASE_STATUS };

static UBYTE  phase;
static ULONG  tag;                  /* the host's tag, echoed in the CSW */
static ULONG  residue;              /* what we did not transfer */
static UBYTE  status;               /* CSW_PASSED and friends */

static ULONG  lba;                  /* the sector being read or written */
static ULONG  blocks_left;
static UWORD  offset;               /* within the sector, in bytes */

static UBYTE  sector[512];      /* SECTOR_SIZE, but an array wants a plain number */  /* one sector, staged */

static const UBYTE *reply;          /* a short answer, sent from memory */
static UWORD  reply_left;

static UBYTE  sense_key;
static UBYTE  sense_asc;

static BOOL   dir_in;               /* the host expects data from us */
static ULONG  ep3_in_pid, ep3_out_pid;
static BOOL   shared;               /* a program has handed the drive over */
/*
 * What has been asked of the drive since the last hand-over.  Counted
 * rather than reasoned about: "the other machine sees no medium" has
 * three quite different causes, and the three numbers tell them apart --
 * nothing arrives, or something arrives and the card refuses, or the
 * card answers and the reply does not get back.
 */
static ULONG  n_commands;
static ULONG  n_reads;
static ULONG  n_failed;
static LONG   last_error;
static ULONG  n_lba_zero;           /* how often sector 0 is asked for */
static ULONG  n_lba_max;            /* and how far the other machine got */
static UWORD  mbr_signature;        /* what sector 0 ended with, as read */
static UBYTE  part_table[64];       /* and its four partition entries */

/*
 * The commands as they arrived, in order, with what we answered.
 *
 * Counting is not enough: a command we refuse never reaches the card, so
 * it raises n_commands and not n_reads, and "commands but no reads" reads
 * the same whether the other machine never asked to read or asked in a
 * dialect we reject.  Those are opposite faults.  So each command's
 * opcode is kept with the additional sense code we refused it with -- 00
 * for one we answered, ff for one still in flight.
 */
/*
 * What the transport itself did, as opposed to what the commands said.
 *
 * Every command was answered CSW_PASSED and the sector in the buffer was
 * right, and the other machine still read sector 0 five times and gave up.
 * So the question is no longer what we decided but whether what we decided
 * ever reached it, and that is several separate things: did the host give
 * up on a transfer and reset us, did it lose its place in the command
 * stream, did every status wrapper go out, and did it take every data
 * packet.  The last one is the only way to tell "the host read our bytes
 * and disliked them" from "the host never got our bytes".
 */
static ULONG  n_resets;             /* bulk-only resets the host asked for */
static ULONG  n_bad_cbw;            /* command blocks that were not one */
static ULONG  n_csw;                /* status wrappers handed over */
static ULONG  n_in;                 /* IN packets the host actually took */
static ULONG  n_dpram_bad;          /* bytes that did not survive the copy */

#define TRACE_MAX   20
static UBYTE  trace_op[TRACE_MAX];
static UBYTE  trace_asc[TRACE_MAX];
static UWORD  trace_n;

static void trace_answered(UBYTE asc)
{
    if (trace_n && trace_n <= TRACE_MAX && trace_asc[trace_n - 1] == 0xff)
        trace_asc[trace_n - 1] = asc;
}

static BOOL   changed;              /* tell the host once, after a handover;
                                       not initialised: .data stays in flash */
static ULONG  capacity;             /* sectors, from the drive itself */

/* ==== the endpoint ===================================================== */

/* Write with the current toggle, then flip it -- the other way round
   and the first packet goes out as DATA1, which the host discards. */
static void ep3_receive(void)
{
    rp2350_usbcon_buf_ctrl(EP3_OUT_BUF_CTRL, BULK_SIZE | ep3_out_pid | BUF_AVAIL);
    ep3_out_pid ^= BUF_DATA1;
}

static void ep3_send(const UBYTE *data, UWORD len)
{
    volatile UBYTE *buf = DPRAM_PTR(EP3_IN_BUF);
    UWORD i;

    for (i = 0; i < len; i++)
        buf[i] = data[i];

    /* Read it back.  Everything else in this path can be reasoned about
       from the datasheet; whether a byte store to the controller's memory
       keeps the byte cannot, and if it does not, every packet leaves here
       wrong while nothing in this driver notices. */
    for (i = 0; i < len; i++)
        if (buf[i] != data[i])
            n_dpram_bad++;

    rp2350_usbcon_buf_ctrl(EP3_IN_BUF_CTRL,
                           len | BUF_FULL | ep3_in_pid | BUF_AVAIL);
    ep3_in_pid ^= BUF_DATA1;
}

static void send_csw(void)
{
    UBYTE csw[CSW_LEN];

    trace_answered(0);      /* no-op if fail() already said why */
    n_csw++;

    csw[0] = (UBYTE)CSW_SIGNATURE;
    csw[1] = (UBYTE)(CSW_SIGNATURE >> 8);
    csw[2] = (UBYTE)(CSW_SIGNATURE >> 16);
    csw[3] = (UBYTE)(CSW_SIGNATURE >> 24);
    csw[4] = (UBYTE)tag;
    csw[5] = (UBYTE)(tag >> 8);
    csw[6] = (UBYTE)(tag >> 16);
    csw[7] = (UBYTE)(tag >> 24);
    csw[8] = (UBYTE)residue;
    csw[9] = (UBYTE)(residue >> 8);
    csw[10] = (UBYTE)(residue >> 16);
    csw[11] = (UBYTE)(residue >> 24);
    csw[12] = status;

    phase = PHASE_STATUS;
    ep3_send(csw, CSW_LEN);
}

/* A short answer -- inquiry data, sense data, a capacity -- sent from
   memory in packets of 64 until it is done. */
static void send_reply(const UBYTE *data, UWORD len, ULONG wanted)
{
    if (len > wanted)
        len = (UWORD)wanted;

    reply = data;
    reply_left = len;
    residue = wanted - len;
    status = CSW_PASSED;

    if (len == 0)
    {
        send_csw();
        return;
    }
    phase = PHASE_DATA_IN;
    ep3_send(reply, (len > BULK_SIZE) ? BULK_SIZE : len);
}

/*
 * A command can fail and still owe a data phase.  The host asked for so
 * many bytes and is waiting for them; answering with the status wrapper
 * instead leaves it waiting, and after a few retries it gives up on the
 * whole device -- which takes the console with it, since they share one
 * interrupt.
 *
 * The transport allows less data than asked for: a short packet tells
 * the host to stop reading and collect the status.  An empty one is the
 * shortest of all, and needs no stall to be cleared afterwards.
 */
static void fail(UBYTE key, UBYTE asc, ULONG wanted)
{
    sense_key = key;
    sense_asc = asc;
    status = CSW_FAILED;
    trace_answered(asc);
    residue = wanted;
    reply_left = 0;
    blocks_left = 0;

    if (wanted && dir_in)
    {
        phase = PHASE_DATA_IN;
        ep3_send(NULL, 0);
        return;
    }
    send_csw();
}

/* ==== the commands ===================================================== */

static const UBYTE inquiry_data[36] = {
    0x00,                   /* direct access block device */
    /*
     * Not removable, although the card plainly is.
     *
     * THIS BIT DECIDES WHETHER PARTITIONS BECOME DRIVES
     *
     * Almost every USB stick reports itself fixed, whatever the socket it
     * lives in, and a host shows it as removable because of the bus it is
     * on rather than because of this bit.  What this bit changes is how
     * the host treats the medium: set, the medium is the floppy-shaped
     * kind, and Windows builds one volume spanning the whole disk -- a
     * "superfloppy" -- rather than a volume for each partition.  Measured
     * with it set: the table is read, and read again, thirty-three times
     * over one session, and not one volume is ever made from it.  The one
     * volume Windows did once make for this device covered the whole disk
     * (its instance was STORAGE\VOLUME\...USBSTOR#DISK&..., not a
     * partition) and was lettered G:.
     *
     * Clear, and the partitions become drives.
     *
     * The reason clearing it failed the first time was not this bit
     * either, and it is worth saying so because both of the obvious
     * readings of that failure were wrong.  A host reads a fixed disk's
     * partition table when it first meets the disk and never again, since
     * a fixed medium cannot change -- and it first met this one while the
     * card still belonged to pTOS, because the interface is announced for
     * as long as the machine is plugged in.  So it read the table of an
     * empty drive.  udr_share() now re-enumerates when it hands the card
     * over (rp2350_usbcon_reattach()), which is the ordinary case of a
     * reader plugged in with a card already in it, so the one look the
     * host takes is a look at the card.
     *
     * A third cause, gone as well: sd_calc_capacity() was short by 1024
     * sectors (bios/sd.c), so the last partition ended past the end of
     * the medium, and no host accepts a table that does not fit the disk
     * it describes.
     */
    0x00,
    0x04,                   /* SPC-2 */
    0x02,                   /* response format */
    31,                     /* what follows */
    0, 0, 0,
    'p', 'T', 'O', 'S', ' ', ' ', ' ', ' ',
    /* Sixteen characters, and they are what the other machine files the
       device under: the flash drive this used to be is gone, and a host
       that has already decided something about "Flash drive" keeps
       deciding it until the name changes. */
    'S', 'D', ' ', 'c', 'a', 'r', 'd', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
    '1', '.', '0', ' '
};

/*
 * What a host asks when it wants to know what this device *is*, as
 * opposed to what it can do.
 *
 * INQUIRY carries a bit -- EVPD -- that changes the question completely.
 * Clear, the host wants the standard data above.  Set, it wants one
 * numbered "vital product data" page and will read whatever comes back
 * as that page.  This driver used to ignore the bit and the page number
 * and answer the standard data to both, so a host asking who we are read
 * a device type, a length and a vendor name as a page header and a
 * designator, and got nonsense it had no way to recognise as nonsense.
 *
 * What that costs is not obvious and was expensive to find.  Windows
 * still builds the drive -- it appears, its sectors read correctly -- but
 * the storage stack will not take it: the drive layout comes back empty,
 * so Win32_DiskDrive.Partitions is blank where every other disk has a
 * number, the disk is missing from the storage service's list while
 * sitting in the legacy one, and no volume is ever made from the
 * partitions.  Nothing is logged and no command fails.
 *
 * Three pages are enough, and a host that asks for a fourth has to be
 * told we do not have it rather than handed something else.
 */
#define VPD_SUPPORTED_PAGES     0x00
#define VPD_UNIT_SERIAL         0x80
#define VPD_DEVICE_ID           0x83

static const UBYTE vpd_supported[] = {
    0x00,                   /* direct access block device, as above */
    VPD_SUPPORTED_PAGES,
    0x00,
    3,                      /* three page numbers follow */
    VPD_SUPPORTED_PAGES, VPD_UNIT_SERIAL, VPD_DEVICE_ID
};

static const UBYTE vpd_serial[] = {
    0x00,
    VPD_UNIT_SERIAL,
    0x00,
    12,                     /* the twelve the transport insists on */
    '0','0','0','0','0','0','0','0','0','0','0','1'
};

/*
 * Identity the T10 way: the vendor's own name, then whatever that vendor
 * cares to add -- here the product and the serial, so the whole string
 * distinguishes this device from anything else called pTOS.
 */
static const UBYTE vpd_device_id[] = {
    0x00,
    VPD_DEVICE_ID,
    0x00, 4 + 36,           /* the descriptor below, with its header */
    0x02,                   /* the identifier is printable ASCII */
    0x01,                   /* and is a T10 vendor identification based one */
    0x00,
    36,
    'p','T','O','S',' ',' ',' ',' ',
    'S','D',' ','c','a','r','d',' ',' ',' ',' ',' ',' ',' ',' ',' ',
    '0','0','0','0','0','0','0','0','0','0','0','1'
};

static void do_inquiry(const UBYTE *cdb, ULONG wanted)
{
    if (cdb[1] & 0x01)          /* EVPD: a numbered page, not the standard data */
    {
        switch (cdb[2])
        {
        case VPD_SUPPORTED_PAGES:
            send_reply(vpd_supported, sizeof(vpd_supported), wanted);
            return;
        case VPD_UNIT_SERIAL:
            send_reply(vpd_serial, sizeof(vpd_serial), wanted);
            return;
        case VPD_DEVICE_ID:
            send_reply(vpd_device_id, sizeof(vpd_device_id), wanted);
            return;
        }
        fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_CDB, wanted);
        return;
    }

    /* Without the bit there is no page to ask for, so a page number here
       means the host meant something this command cannot express. */
    if (cdb[2] != 0)
    {
        fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_CDB, wanted);
        return;
    }

    send_reply(inquiry_data, sizeof(inquiry_data), wanted);
}

static void do_request_sense(ULONG wanted)
{
    static UBYTE sense[18];

    memset(sense, 0, sizeof(sense));
    sense[0] = 0x70;                /* current error, fixed format */
    sense[2] = sense_key;
    sense[7] = 10;                  /* additional length */
    sense[12] = sense_asc;

    /* Reading the sense clears it, as the standard wants. */
    sense_key = SENSE_NONE;
    sense_asc = 0;

    send_reply(sense, sizeof(sense), wanted);
}

static void do_read_capacity(ULONG wanted)
{
    static UBYTE cap[8];
    ULONG last = capacity - 1;

    cap[0] = (UBYTE)(last >> 24);
    cap[1] = (UBYTE)(last >> 16);
    cap[2] = (UBYTE)(last >> 8);
    cap[3] = (UBYTE)last;
    cap[4] = 0;
    cap[5] = 0;
    cap[6] = (UBYTE)(SECTOR_SIZE >> 8);
    cap[7] = (UBYTE)SECTOR_SIZE;

    send_reply(cap, sizeof(cap), wanted);
}

/* Windows asks for this before it will mount removable media. */
static void do_read_format_capacities(ULONG wanted)
{
    static UBYTE fc[12];

    memset(fc, 0, sizeof(fc));
    fc[3] = 8;                      /* one descriptor follows */
    fc[4] = (UBYTE)(capacity >> 24);
    fc[5] = (UBYTE)(capacity >> 16);
    fc[6] = (UBYTE)(capacity >> 8);
    fc[7] = (UBYTE)capacity;
    fc[8] = 0x02;                   /* formatted media */
    fc[9] = 0;
    fc[10] = (UBYTE)(SECTOR_SIZE >> 8);
    fc[11] = (UBYTE)SECTOR_SIZE;

    send_reply(fc, sizeof(fc), wanted);
}

/*
 * The ten-byte form, which is the one a fixed disk is asked for.  The
 * header is four bytes longer and the length field is two bytes wide;
 * everything else says the same thing as the six-byte form below.
 */
static void do_mode_sense_10(ULONG wanted)
{
    static UBYTE mode[8];

    memset(mode, 0, sizeof(mode));
    mode[0] = 0;                    /* length that follows, high byte */
    mode[1] = 6;                    /* and low */
    mode[2] = 0;                    /* medium type */
    mode[3] = 0;                    /* not write protected */
    /* [4..5] reserved, [6..7] no block descriptors */

    send_reply(mode, sizeof(mode), wanted);
}

/*
 * The sixteen-byte capacity.  A host that is content with the ten-byte
 * answer never asks, but one that does ask and is refused can decide the
 * device is too old to be trusted with the disk.
 */
static void do_read_capacity_16(ULONG wanted)
{
    static UBYTE cap[32];
    ULONG last = capacity - 1;

    memset(cap, 0, sizeof(cap));
    cap[4] = (UBYTE)(last >> 24);   /* [0..3] stay zero: no card is that big */
    cap[5] = (UBYTE)(last >> 16);
    cap[6] = (UBYTE)(last >> 8);
    cap[7] = (UBYTE)last;
    cap[10] = (UBYTE)(SECTOR_SIZE >> 8);
    cap[11] = (UBYTE)SECTOR_SIZE;

    send_reply(cap, sizeof(cap), wanted);
}

static void do_mode_sense(ULONG wanted)
{
    static UBYTE mode[4];

    mode[0] = 3;                    /* length that follows */
    mode[1] = 0;                    /* medium type */
    mode[2] = 0;                    /* not write protected */
    mode[3] = 0;                    /* no block descriptors */

    send_reply(mode, sizeof(mode), wanted);
}

/* Read one sector into the staging buffer and start sending it. */
static void read_next_sector(void)
{
    LONG ret;

    n_reads++;
    if (lba == 0)
        n_lba_zero++;
    if (lba > n_lba_max)
        n_lba_max = lba;
    ret = sd_rw(0, (LONG)lba, 1, sector, 0);
    if (ret == 0 && lba == 0)
    {
        mbr_signature = (UWORD)(((UWORD)sector[511] << 8) | sector[510]);
        memcpy(part_table, sector + 0x1be, sizeof(part_table));
    }
    if (ret != 0)
    {
        n_failed++;
        last_error = ret;
        status = CSW_FAILED;
        sense_key = SENSE_NOT_READY;
        sense_asc = ASC_MEDIUM_NOT_PRESENT;
        send_csw();
        return;
    }
    offset = 0;
    ep3_send(sector, BULK_SIZE);
}

static void start_read(const UBYTE *cdb, ULONG wanted)
{
    lba = ((ULONG)cdb[2] << 24) | ((ULONG)cdb[3] << 16)
        | ((ULONG)cdb[4] << 8) | cdb[5];
    blocks_left = ((ULONG)cdb[7] << 8) | cdb[8];

    if (lba + blocks_left > capacity)
    {
        fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_CDB, wanted);
        return;
    }
    if (blocks_left == 0)
    {
        status = CSW_PASSED;
        residue = wanted;
        send_csw();
        return;
    }

    status = CSW_PASSED;
    residue = wanted;
    phase = PHASE_DATA_IN;
    read_next_sector();
}

static void start_write(const UBYTE *cdb, ULONG wanted)
{
    lba = ((ULONG)cdb[2] << 24) | ((ULONG)cdb[3] << 16)
        | ((ULONG)cdb[4] << 8) | cdb[5];
    blocks_left = ((ULONG)cdb[7] << 8) | cdb[8];

    if (lba + blocks_left > capacity)
    {
        fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_CDB, wanted);
        return;
    }
    if (blocks_left == 0)
    {
        status = CSW_PASSED;
        residue = wanted;
        send_csw();
        return;
    }

    status = CSW_PASSED;
    residue = wanted;
    offset = 0;
    phase = PHASE_DATA_OUT;
    ep3_receive();
}

/* ==== the command wrapper ============================================== */

static void handle_cbw(const volatile UBYTE *p, UWORD len)
{
    UBYTE cdb[16];
    ULONG signature, wanted;
    int   i;

    signature = (ULONG)p[0] | ((ULONG)p[1] << 8)
              | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);

    if (len != CBW_LEN || signature != CBW_SIGNATURE)
    {
        /* Not a wrapper.  Stall until the host resets us. */
        n_bad_cbw++;
        BUF_CTRL(EP3_OUT_BUF_CTRL) = BUF_STALL;
        BUF_CTRL(EP3_IN_BUF_CTRL) = BUF_STALL;
        return;
    }

    tag = (ULONG)p[4] | ((ULONG)p[5] << 8)
        | ((ULONG)p[6] << 16) | ((ULONG)p[7] << 24);
    wanted = (ULONG)p[8] | ((ULONG)p[9] << 8)
           | ((ULONG)p[10] << 16) | ((ULONG)p[11] << 24);

    for (i = 0; i < 16; i++)
        cdb[i] = p[15 + i];

    dir_in = (p[12] & 0x80) ? TRUE : FALSE;
    residue = wanted;
    status = CSW_PASSED;
    n_commands++;

    if (trace_n < TRACE_MAX)
    {
        trace_op[trace_n] = cdb[0];
        trace_asc[trace_n] = 0xff;      /* not answered yet */
        trace_n++;
    }

    /* Nothing that touches the medium answers while it is ours. */
    if (!shared)
    {
        switch (cdb[0])
        {
        case SCSI_INQUIRY:
            do_inquiry(cdb, wanted);
            return;
        case SCSI_REQUEST_SENSE:
            do_request_sense(wanted);
            return;
        default:
            fail(SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, wanted);
            return;
        }
    }

    /* Once after a handover, say so: the host then forgets what it
       thought it knew about the contents. */
    if (changed && cdb[0] != SCSI_INQUIRY && cdb[0] != SCSI_REQUEST_SENSE)
    {
        changed = FALSE;
        fail(SENSE_UNIT_ATTENTION, ASC_MEDIUM_CHANGED, wanted);
        return;
    }

    switch (cdb[0])
    {
    case SCSI_TEST_UNIT_READY:
    case SCSI_PREVENT_ALLOW_REMOVAL:
    case SCSI_START_STOP_UNIT:
    case SCSI_VERIFY_10:
    /* Every write is already on the card before its status wrapper goes
       out (start_write), so there is nothing held back to flush. */
    case SCSI_SYNCHRONIZE_CACHE_10:
        residue = wanted;
        send_csw();
        break;
    case SCSI_INQUIRY:
        do_inquiry(cdb, wanted);
        break;
    case SCSI_REQUEST_SENSE:
        do_request_sense(wanted);
        break;
    case SCSI_MODE_SENSE_6:
        do_mode_sense(wanted);
        break;
    case SCSI_MODE_SENSE_10:
        do_mode_sense_10(wanted);
        break;
    case SCSI_SERVICE_ACTION_IN_16:
        if ((cdb[1] & 0x1f) == SAI_READ_CAPACITY_16)
            do_read_capacity_16(wanted);
        else
            fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_CDB, wanted);
        break;
    case SCSI_READ_CAPACITY_10:
        do_read_capacity(wanted);
        break;
    case SCSI_READ_FORMAT_CAPACITIES:
        do_read_format_capacities(wanted);
        break;
    case SCSI_READ_10:
        start_read(cdb, wanted);
        break;
    case SCSI_WRITE_10:
        start_write(cdb, wanted);
        break;
    default:
        fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, wanted);
        break;
    }
}

/* ==== what the interrupt calls ========================================= */

/* A packet arrived on the drive's OUT endpoint. */
void rp2350_usbmsc_out(void)
{
    volatile UBYTE *buf = DPRAM_PTR(EP3_OUT_BUF);
    UWORD len = (UWORD)(BUF_CTRL(EP3_OUT_BUF_CTRL) & BUF_LEN_MASK);
    UWORD i;

    if (phase == PHASE_DATA_OUT)
    {
        for (i = 0; i < len && offset < SECTOR_SIZE; i++)
            sector[offset++] = buf[i];

        if (offset >= SECTOR_SIZE)
        {
            /* A whole sector: commit it.  The write blocks interrupts
               for as long as the flash needs, which is why sharing is
               exclusive -- nothing else is expected to be running. */
            if ((last_error = sd_rw(RW_WRITE, (LONG)lba, 1, sector, 0)) != 0)
            {
                n_failed++;
                status = CSW_FAILED;
                sense_key = SENSE_NOT_READY;
                sense_asc = ASC_MEDIUM_NOT_PRESENT;
                send_csw();
                return;
            }
            residue -= SECTOR_SIZE;
            lba++;
            offset = 0;
            if (--blocks_left == 0)
            {
                send_csw();
                return;
            }
        }
        ep3_receive();
        return;
    }

    handle_cbw(buf, len);

    /* A command that needs no data phase has already answered; take the
       next wrapper either way once the answer is on its way. */
    if (phase == PHASE_COMMAND)
        ep3_receive();
}

/* A packet we sent has left. */
void rp2350_usbmsc_in_done(void)
{
    UWORD chunk;

    /* The controller calls this when a packet has gone out and the host
       has acknowledged it -- so this counts bytes the host really took,
       which is the one thing no amount of reading this file can settle. */
    n_in++;

    switch (phase)
    {
    case PHASE_STATUS:
        phase = PHASE_COMMAND;
        ep3_receive();
        break;

    case PHASE_DATA_IN:
        if (reply_left)
        {
            /* a short answer from memory */
            chunk = (reply_left > BULK_SIZE) ? BULK_SIZE : reply_left;
            reply += chunk;
            reply_left -= chunk;
            if (reply_left)
                ep3_send(reply, (reply_left > BULK_SIZE) ? BULK_SIZE : reply_left);
            else
                send_csw();
            break;
        }
        if (blocks_left == 0)
        {
            /* the empty packet of a failure, or nothing left to send */
            send_csw();
            break;
        }

        /* sector data */
        offset += BULK_SIZE;
        residue -= BULK_SIZE;
        if (offset < SECTOR_SIZE)
        {
            ep3_send(sector + offset, BULK_SIZE);
            break;
        }
        lba++;
        if (--blocks_left == 0)
            send_csw();
        else
            read_next_sector();
        break;

    default:
        break;
    }
}

/* Get Max LUN and Bulk-Only Mass Storage Reset, the two class requests. */
BOOL rp2350_usbmsc_request(UBYTE bmRequestType, UBYTE bRequest, UWORD wValue,
                           UWORD wIndex, UWORD wLength)
{
    static const UBYTE zero = 0;

    (void)wValue;
    (void)wIndex;

    if ((bmRequestType & 0x7f) != 0x21)      /* class, to an interface */
        return FALSE;

    if (bRequest == 0xfe && wLength >= 1)    /* Get Max LUN: one drive */
    {
        rp2350_usbcon_ep0_send(&zero, 1, wLength);
        return TRUE;
    }
    if (bRequest == 0xff)                    /* reset the transport */
    {
        /* The host only asks for this when a transfer did not finish, so
           this counter answers "did it give up on us" outright. */
        n_resets++;
        rp2350_usbmsc_reset();
        rp2350_usbcon_ep0_send(NULL, 0, 0);
        return TRUE;
    }
    return FALSE;
}

void rp2350_usbmsc_reset(void)
{
    ep3_in_pid = 0;
    ep3_out_pid = 0;
    phase = PHASE_COMMAND;
    reply_left = 0;
    blocks_left = 0;
    ep3_receive();
}

/*
 * How big the card is, asked of the card.  Not remembered from start-up:
 * USB is configured before the disks are looked at, and a card put in
 * later would be 0 sectors for good.
 */
static ULONG card_sectors(void)
{
    ULONG info[2];

    if (sd_ioctl(0, GET_DISKINFO, info) != 0)
        return 0;
    return info[0];
}

void rp2350_usbmsc_init(void)
{
    /*
     * Ask the card how big it is, but only while it is still ours.
     *
     * This runs from SET_CONFIGURATION, which is to say from the USB
     * interrupt in the middle of the host enumerating us -- and since the
     * handover re-enumerates on purpose (rp2350_usbcon_reattach()), that
     * now happens while the card is lent out and udr_share() has already
     * established the capacity.  Asking again there can only do harm: if
     * the card does not answer in that context, card_sectors() returns 0,
     * every read is then refused by start_read() as a request past the end
     * of a zero-length medium, and the host concludes the disk has no
     * partition table.  Which is exactly what it concluded.
     */
    if (!shared)
        capacity = card_sectors();

    EP_CTRL(EP3_IN_CTRL) = EP_CTRL_ENABLE | EP_CTRL_INT_PER_BUFF
                         | EP_CTRL_TYPE_BULK | EP3_IN_BUF;
    EP_CTRL(EP3_OUT_CTRL) = EP_CTRL_ENABLE | EP_CTRL_INT_PER_BUFF
                          | EP_CTRL_TYPE_BULK | EP3_OUT_BUF;

    rp2350_usbmsc_reset();
}

/* ==== what a program calls ============================================= */

/*
 * Hand the drive to the other machine, or take it back.  Exclusive on
 * purpose: two file systems with their own caches on one medium corrupt
 * it.  The drive is reported as changed in both directions, so whoever
 * gets it throws away what they thought they knew.
 */
LONG rp2350_usbmsc_share(WORD on)
{
    LONG ret;

    /*
     * Not while a program has the console.  That is a deploy in flight,
     * and it is writing to a drive on this very card: handing the card
     * over underneath it would pull the medium out from under GEMDOS's
     * buffers.  The two are locked against each other at the resource
     * rather than in the menus, so that no arrangement of clicks can get
     * between them.
     */
    if (on && rp2350_usbcon_is_raw())
        return EACCDN;

    capacity = card_sectors();
    if (capacity == 0)
        return EUNDEV;

    /* The card changes hands: pTOS lets go of every drive on it, and
       reports the medium as changed so that GEMDOS drops what it was
       holding.  See the SDMMC_BUS cases in disk.c. */
    ret = disk_lend_sd(on);
    if (ret != E_OK)
        return ret;

    /*
     * Prove the card can still be read now that pTOS has let go of it --
     * here, in the caller's own context.  Every later read comes out of
     * the USB interrupt instead, so if this succeeds and the other
     * machine still sees no medium, the difference is the context and
     * not the card.
     */
    if (on && sd_rw(0, 0L, 1, sector, 0) != 0)
    {
        disk_lend_sd(0);
        return EREADF;
    }

    shared = on ? TRUE : FALSE;

    /*
     * "The medium changed" is something to tell a host that already knows
     * what the medium was.  Handing the card over re-enumerates below, so
     * the host that asks afterwards has never seen this drive and has
     * nothing to be told: answering its very first TEST UNIT READY with
     * UNIT ATTENTION lands in the middle of its initialisation, where a
     * host is entitled to read it as "no medium" and stop -- and then it
     * does not read the partition table, because it has just decided
     * there is nothing to read.  That was measured: with the handover
     * announced this way, partmgr found no partitions at all, where
     * without the re-enumeration it had found both.
     *
     * Taking the card back is the other case, and there it is exactly
     * right: the host is still enumerated, it knew the medium, and the
     * medium is gone.
     */
    changed = on ? FALSE : TRUE;
    n_commands = n_reads = n_failed = 0;
    n_lba_zero = n_lba_max = 0;
    mbr_signature = 0;
    memset(part_table, 0, sizeof(part_table));
    trace_n = 0;
    memset(trace_op, 0, sizeof(trace_op));
    memset(trace_asc, 0, sizeof(trace_asc));
    n_resets = n_bad_cbw = n_csw = n_in = n_dpram_bad = 0;

    last_error = 0;
    mbr_signature = 0;
    if (on)
    {
        sense_key = SENSE_NONE;     /* nothing to report to a new host */
        sense_asc = 0;
    }
    else
    {
        sense_key = SENSE_UNIT_ATTENTION;
        sense_asc = ASC_MEDIUM_CHANGED;
    }

    /*
     * Make the host enumerate the drive again, now that the medium is
     * here.  A host reads a disk's partition table when it first meets
     * the disk, and it first met this one when the card was still ours
     * and every command was answered "medium not present".  Telling it
     * afterwards that the medium changed is not reliably enough: on
     * Windows 10 the partitions are then found and no volume is ever
     * built from them.  Coming back as a new device is, because it is
     * the ordinary case of a card reader plugged in with a card in it.
     *
     * Only when handing the card over.  Taking it back is a medium going
     * away, which is what the removable bit is for and what every host
     * handles; and each re-enumeration costs the console a moment.
     */
    if (on)
        rp2350_usbcon_reattach();

    return 0;
}

BOOL rp2350_usbmsc_shared(void)
{
    return shared;
}

/* ==== what the cookie hands out ======================================== */

static long udr_share(short on)
{
    return rp2350_usbmsc_share((WORD)on);
}

static short udr_shared(void)
{
    return shared ? 1 : 0;
}

static long udr_sectors(void)
{
    return (long)card_sectors();
}

/*
 * Only as far as the caller has room for: the struct has grown once and
 * a program built against the older header must not be written past.
 */
static void udr_stats(struct udr_stats *s, UWORD size)
{
    struct udr_stats full;

    if (!s || size == 0)
        return;

    full.commands = n_commands;
    full.reads = n_reads;
    full.failed = n_failed;
    full.last_error = last_error;
    full.lba_zero = n_lba_zero;
    full.lba_max = n_lba_max;
    full.signature = mbr_signature;
    full.capacity = capacity;
    memcpy(full.part, part_table, sizeof(full.part));
    full.resets = n_resets;
    full.bad_cbw = n_bad_cbw;
    full.csw = n_csw;
    full.in_packets = n_in;
    full.dpram_bad = n_dpram_bad;
    full.traced = trace_n;
    memcpy(full.trace_op, trace_op, sizeof(full.trace_op));
    memcpy(full.trace_asc, trace_asc, sizeof(full.trace_asc));

    if (size > (UWORD)sizeof(full))
        size = (UWORD)sizeof(full);
    memcpy(s, &full, size);
}

static const struct udr_api udr_api = {
    UDR_API_VERSION,
    sizeof(struct udr_api),
    udr_share,
    udr_shared,
    udr_sectors,
    udr_stats
};

/* Always: whether there is a drive to share is asked when a program
   wants to share it, by which time the drive is mounted.  At the time
   the cookie jar is filled it is not yet. */
void rp2350_usbmsc_add_cookie(void)
{
    cookie_add(UDR_COOKIE, (ULONG)&udr_api);
}

#endif /* CONF_WITH_RP2350_USBMSC */
