/*
 * rp2350_usbmsc.c - the flash drive as a USB disk, when a program shares it
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This software is licenced under the GNU Public License.
 * Please see LICENSE.TXT for further information.
 *
 * The device already offers a serial console on its one USB port; this
 * adds a second function beside it, a disk, so that F: can be opened in
 * a file manager on the other machine.
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
#include "rp2350_flashdisk.h"
#include "biosdefs.h"
#include "disk.h"
#include "gemerror.h"
#include "string.h"
#include "biosext.h"
#include "cookie.h"
#include "usbdrv.h"

#if CONF_WITH_RP2350_FLASHDISK

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

    rp2350_usbcon_buf_ctrl(EP3_IN_BUF_CTRL,
                           len | BUF_FULL | ep3_in_pid | BUF_AVAIL);
    ep3_in_pid ^= BUF_DATA1;
}

static void send_csw(void)
{
    UBYTE csw[CSW_LEN];

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
    0x80,                   /* removable: it can be taken away, and is */
    0x04,                   /* SPC-2 */
    0x02,                   /* response format */
    31,                     /* what follows */
    0, 0, 0,
    'p', 'T', 'O', 'S', ' ', ' ', ' ', ' ',
    'F', 'l', 'a', 's', 'h', ' ', 'd', 'r', 'i', 'v', 'e', ' ', ' ', ' ', ' ', ' ',
    '1', '.', '0', ' '
};

static void do_inquiry(ULONG wanted)
{
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
    if (rp2350_flashdisk_usb_rw(0, (LONG)lba, 1, sector) != 0)
    {
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

    /* Nothing that touches the medium answers while it is ours. */
    if (!shared)
    {
        switch (cdb[0])
        {
        case SCSI_INQUIRY:
            do_inquiry(wanted);
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
        residue = wanted;
        send_csw();
        break;
    case SCSI_INQUIRY:
        do_inquiry(wanted);
        break;
    case SCSI_REQUEST_SENSE:
        do_request_sense(wanted);
        break;
    case SCSI_MODE_SENSE_6:
        do_mode_sense(wanted);
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
            if (rp2350_flashdisk_usb_rw(RW_WRITE, (LONG)lba, 1, sector) != 0)
            {
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

void rp2350_usbmsc_init(void)
{
    capacity = (ULONG)rp2350_flashdisk_sectors();

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

    /* Asked now, not remembered from start-up: the cookie jar is filled
       and USB configured before the flash drive is mounted, and a size
       taken then would be 0 for good. */
    capacity = (ULONG)rp2350_flashdisk_sectors();
    if (capacity == 0)
        return EUNDEV;

    /* The drive changes hands: pTOS lets go, and reports the medium as
       changed so that GEMDOS drops what it was holding. */
    ret = rp2350_flashdisk_set_shared(on);
    if (ret != E_OK)
        return ret;

    shared = on ? TRUE : FALSE;
    changed = TRUE;
    sense_key = SENSE_UNIT_ATTENTION;
    sense_asc = ASC_MEDIUM_CHANGED;
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
    return rp2350_flashdisk_sectors();
}

static const struct udr_api udr_api = {
    UDR_API_VERSION,
    sizeof(struct udr_api),
    udr_share,
    udr_shared,
    udr_sectors
};

/* Always: whether there is a drive to share is asked when a program
   wants to share it, by which time the drive is mounted.  At the time
   the cookie jar is filled it is not yet. */
void rp2350_usbmsc_add_cookie(void)
{
    cookie_add(UDR_COOKIE, (ULONG)&udr_api);
}

#endif /* CONF_WITH_RP2350_FLASHDISK */
