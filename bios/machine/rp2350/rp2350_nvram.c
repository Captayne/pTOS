/*
 * rp2350_nvram.c - the settings the machine keeps when the power goes
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Two sectors at the top of the flash, written alternately.  Each holds
 * one record: a header saying what it is, how long it is and which of the
 * two is the newer, and then the settings themselves.
 *
 * NOT A DRIVE, AND NOT REACHABLE FROM A PROGRAM
 *
 * There is no drive letter, no file system and no BIOS call to get at
 * this.  Nothing outside the operating system can see it, which is the
 * whole point: a drive in the flash meant a file system writing whenever
 * it felt like it, and every write to this flash takes both QSPI chip
 * selects away for 23 milliseconds -- the PSRAM with it (rp2350_flash.c).
 * The system chooses when to pay that, and it pays it about as often as
 * somebody changes a setting.
 *
 * Reading costs nothing at all: the record is in the memory-mapped XIP
 * window and is read like any other memory.
 *
 * WHY TWO SECTORS
 *
 * Because flash is erased in blocks and the power can go in the middle.
 * Writing means erasing the sector that does *not* hold the current
 * record and building the new one there; the old one is untouched until
 * the new one is whole and carries a higher sequence number.  There is no
 * moment when neither is valid, so an interrupted save costs the change
 * being made and never what was already kept.
 *
 * WHY A VERSION
 *
 * Because the settings will grow, and an image that cannot read what the
 * image before it wrote loses everything on every update.  The reader
 * takes the length from the record rather than from its own idea of it,
 * and a shorter record is one from an older version: what it carries is
 * kept and the rest takes its default.  So fields may be added at the end
 * and never moved or re-used.
 */

#include "emutos.h"
#include "rp2350.h"
#include "rp2350_flash.h"
#include "rp2350_nvram.h"
#include "gemerror.h"
#include "string.h"
#include "tosvars.h"        /* phystop, for what AUTO means here */
#include "kprint.h"

#if CONF_WITH_RP2350_NVRAM

/*
 * The last two 4 KB sectors of the 16 MB flash.  At the top because the
 * image grows upwards from zero and the drive that used to live at
 * 0x900000 is gone: nothing here moves when pTOS gets bigger.
 */
#define SECTOR_SIZE     4096UL
#define FLASH_SIZE      0x01000000UL
#define SLOT_A_OFFSET   (FLASH_SIZE - 2 * SECTOR_SIZE)
#define SLOT_B_OFFSET   (FLASH_SIZE - 1 * SECTOR_SIZE)

#define XIP(offs)       ((const struct record *)(0x10000000UL + (offs)))

#define NVRAM_MAGIC     0x50534554UL    /* 'PSET' */

/*
 * What sits at the start of a sector.  The length is the length of the
 * settings that follow, so that a record written by an older image is
 * read as far as it goes and no further.
 */
struct record {
    ULONG   magic;
    ULONG   sequence;       /* the higher of the two is the live one */
    ULONG   length;         /* bytes of settings after this header */
    ULONG   sum;            /* over those bytes */
    struct rp2350_settings  settings;
};

/* the settings as they stand, and which sector they came from */
static struct rp2350_settings live;
static WORD live_slot = -1;         /* -1: nothing valid was found */
static ULONG live_sequence;

/*
 * Not a strong check and not meant to be: it is here to notice a record
 * that was half written, not one that somebody tampered with.  A sum of
 * words with the length folded in catches a short write, which is the
 * failure that can actually happen here.
 */
static ULONG checksum(const UBYTE *p, ULONG count)
{
    ULONG sum = count;
    ULONG i;

    for (i = 0; i < count; i++)
        sum = (sum << 1) + (sum >> 31) + p[i];
    return sum;
}

static BOOL record_valid(const struct record *r)
{
    if (r->magic != NVRAM_MAGIC)
        return FALSE;
    if (r->length == 0 || r->length > SECTOR_SIZE - sizeof(struct record)
                                    + sizeof(struct rp2350_settings))
        return FALSE;
    return checksum((const UBYTE *)&r->settings, r->length) == r->sum;
}

/*
 * Take what a record carries and leave the rest at its default.  This is
 * where an older, shorter record is made welcome.
 */
static void adopt(const struct record *r)
{
    ULONG take = r->length;

    if (take > sizeof(live))
        take = sizeof(live);        /* newer image wrote more than we know */

    memset(&live, 0, sizeof(live));
    rp2350_nvram_defaults(&live);
    memcpy(&live, &r->settings, take);
}

/*
 * What a machine that has never been told anything believes.  Separate
 * from the reader because it is also what fills the part of a record an
 * older image did not write.
 */
void rp2350_nvram_defaults(struct rp2350_settings *s)
{
    memset(s, 0, sizeof(*s));
    s->version = RP2350_SETTINGS_VERSION;

    /*
     * Not zero: zero is GPIO 0, a pin a real board uses.  What an
     * untold machine believes about its own wiring is what it was
     * built believing.
     */
#if CONF_WITH_RP2350_PSRAM
    s->psram_cs = CONF_RP2350_PSRAM_CS_PIN;
#endif

#if CONF_WITH_RP2350_LCD
    s->scr_w = CONF_RP2350_LCD_WIDTH;
    s->scr_h = CONF_RP2350_LCD_HEIGHT;
    s->bpp = 16;                /* RGB565: the only format there is a driver for */
#endif
    s->refresh = 0;             /* an SPI panel has no clock to programme */
    s->vram_where = VRAM_AUTO;

    /* strncpy, except that there is no libc here */
    {
        const char *p = CONF_BOARD_NAME;
        int i;

        for (i = 0; i < (int)sizeof(s->board) && p[i]; i++)
            s->board[i] = p[i];
    }
}

/*
 * The bytes a framebuffer needs.  Computed, never kept: see the header.
 * The 2 KB on the end are not slack -- every version of Atari TOS left
 * them there, and programs write past the screen relying on it
 * (bios/screen.c).
 */
ULONG rp2350_nvram_vram_size(void)
{
    ULONG w = live.scr_w, h = live.scr_h, bpp = live.bpp;

    /*
     * Never 2 KB.  A framebuffer that small is not a smaller screen, it
     * is memory the VDI walks straight out of, and the machine dies
     * drawing its own boot screen.  If the settings say nothing, the
     * answer is the screen this image was built for.
     */
    if (!w || !h || !bpp)
    {
#if CONF_WITH_RP2350_LCD
        w = CONF_RP2350_LCD_WIDTH;
        h = CONF_RP2350_LCD_HEIGHT;
        bpp = 16;
#else
        return 2048UL;
#endif
    }

    return w * h * ((bpp + 7) / 8) + 2048UL;
}

/*
 * What AUTO means here.  The SRAM while the framebuffer fits and leaves
 * enough behind for programs to run in, the PSRAM otherwise.
 *
 * The margin is the whole point.  "Fits" is the wrong test: a 320x240
 * framebuffer fits into 456 KB of ST-RAM and leaves 306, which is a
 * machine; a 400x800 one fits too and leaves nothing, which is not.
 */
#define VRAM_STRAM_MARGIN   (256UL * 1024UL)

UBYTE rp2350_nvram_vram_where(void)
{
    ULONG need, stram;

    if (live.vram_where != VRAM_AUTO)
        return live.vram_where;

    need = rp2350_nvram_vram_size();
    stram = (ULONG)phystop;

    if (need + VRAM_STRAM_MARGIN <= stram)
        return VRAM_STRAM;

    return VRAM_PSRAM;
}

/*
 * The name, always terminated, whatever the record holds.  A field that
 * fills every byte is legal and carries no NUL, so it cannot be handed
 * out as it stands.
 */
const char *rp2350_nvram_board(void)
{
    static char name[sizeof(live.board) + 1];
    int i;

    for (i = 0; i < (int)sizeof(live.board); i++)
        name[i] = live.board[i];
    name[i] = '\0';

    return name[0] ? name : CONF_BOARD_NAME;
}

void rp2350_nvram_init(void)
{
    const struct record *a = XIP(SLOT_A_OFFSET);
    const struct record *b = XIP(SLOT_B_OFFSET);
    BOOL ok_a, ok_b;

    rp2350_nvram_defaults(&live);
    live_slot = -1;
    live_sequence = 0;

    if (!rp2350_flash_init())
    {
        KINFO(("nvram: no bootrom flash functions; settings are defaults\n"));
        return;
    }

    ok_a = record_valid(a);
    ok_b = record_valid(b);

    if (ok_a && (!ok_b || a->sequence >= b->sequence))
    {
        adopt(a);
        live_slot = 0;
        live_sequence = a->sequence;
    }
    else if (ok_b)
    {
        adopt(b);
        live_slot = 1;
        live_sequence = b->sequence;
    }

    if (live_slot < 0)
        KINFO(("nvram: nothing kept yet; settings are defaults\n"));
    else
        KINFO(("nvram: slot %c, sequence %lu, %lu bytes\n",
               live_slot ? 'B' : 'A', live_sequence,
               (ULONG)(live_slot ? b->length : a->length)));
}

/*
 * Which sector the settings came from, and how many times they have been
 * kept.  For the boot screen: a machine that has never been told anything
 * and one whose store has gone bad both run on defaults, and it is worth
 * being able to tell them apart without a console.
 */
WORD rp2350_nvram_slot(void)
{
    return live_slot;
}

ULONG rp2350_nvram_sequence(void)
{
    return live_sequence;
}

const struct rp2350_settings *rp2350_nvram_get(void)
{
    return &live;
}

/*
 * Keep them.  The sector that does not hold the current record is erased
 * and the new one built there, so what is already kept survives whatever
 * happens during this.
 *
 * The cost is one erase and one program: a single stretch of about 23
 * milliseconds in which no interrupt is served and neither QSPI chip
 * select answers.  That is why this is not called by anything that runs
 * while a program does -- see the comment at the top of the file.
 */
LONG rp2350_nvram_put(const struct rp2350_settings *s)
{
    static UBYTE page[sizeof(struct record)];
    struct record *r = (struct record *)page;
    WORD slot = (live_slot == 0) ? 1 : 0;
    ULONG offs = slot ? SLOT_B_OFFSET : SLOT_A_OFFSET;

    if (!rp2350_flash_init())
        return EWRPRO;

    memset(page, 0, sizeof(page));
    r->magic = NVRAM_MAGIC;
    r->sequence = live_sequence + 1;
    r->length = sizeof(struct rp2350_settings);
    memcpy(&r->settings, s, sizeof(struct rp2350_settings));
    r->sum = checksum((const UBYTE *)&r->settings, r->length);

    rp2350_flash_erase(offs, SECTOR_SIZE);
    rp2350_flash_program(offs, page, sizeof(page));

    if (!record_valid(XIP(offs)))
        return EWRPRO;              /* it did not take; the old one stands */

    live = *s;
    live_slot = slot;
    live_sequence = r->sequence;
    return E_OK;
}

#endif /* CONF_WITH_RP2350_NVRAM */
