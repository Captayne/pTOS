/*
 * desktouch.c - touch screen calibration, "Touch calibration..." in the
 *               desktop's Options menu
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Nine crosses in a 3 x 3 grid are touched in turn; a least-squares fit
 * (tch_fit() in include/touch.h) turns their raw readings into the
 * calibration, which is used at once and can be saved to C:\TOUCHCAL.INF.
 * The desktop loads that file when it starts, unless the touch screen has
 * just been calibrated by hand at boot.
 *
 * Everything goes through the touch driver's _TCH cookie.  The crosses are
 * drawn as a small object tree: a white box over the whole screen, a line
 * of text, two one-pixel bars and a little square.
 */

#include "emutos.h"
#include "string.h"
#include "obdefs.h"
#include "aesdefs.h"
#include "aesbind.h"
#include "gemdos.h"
#include "desk_rsc.h"
#include "touch.h"
#include "desktouch.h"

#if CONF_WITH_TOUCH_CALIBRATION

#define CAL_FILE        "C:\\TOUCHCAL.INF"
#define CAL_MAGIC       0x54434831L     /* 'TCH1' */

#define NPOINTS         9
#define CROSS           12              /* half the length of a cross bar */

struct cal_file
{
    LONG magic;
    struct tch_cal cal;
};

static struct tch_api *tch;
static WORD scr_w, scr_h;
static short points[NPOINTS][2];

/* the object tree drawing a cross */
enum { O_ROOT, O_TEXT, O_COUNT, O_HBAR, O_VBAR, O_SQUARE, O_NUM };
static OBJECT tree[O_NUM];
static char count_text[16];

#define SSYSTEM         0x154           /* GEMDOS Ssystem() */
#define S_GETCOOKIE     8

static BOOL touch_present(void)
{
    /* Ssystem(), not Supexec(): the latter does not exist on ARM */
    if (!tch)
    {
        ULONG value;

        if (trap1(SSYSTEM, S_GETCOOKIE, TCH_COOKIE, &value) == 0)
            tch = (struct tch_api *)value;
    }
    if (!tch || tch->version < TCH_VERSION)
        return FALSE;
    scr_w = tch->width;
    scr_h = tch->height;
    return TRUE;
}

/* ---- the calibration file ---- */

void touch_load_calibration(void)
{
    struct cal_file f;
    struct tch_cal c;
    LONG fh;

    if (!touch_present())
        return;
    if (tch->get_cal(&c) & TCH_BOOTCAL)     /* just calibrated by hand */
        return;

    fh = dos_open(CAL_FILE, 0);
    if (fh < 0)
        return;
    if (dos_read((WORD)fh, sizeof(f), &f) == sizeof(f)
        && f.magic == CAL_MAGIC && f.cal.div != 0)
        tch->set_cal(&f.cal);
    dos_close((WORD)fh);
}

static BOOL save_calibration(void)
{
    struct cal_file f;
    LONG fh, n;

    f.magic = CAL_MAGIC;
    tch->get_cal(&f.cal);
    fh = dos_create(CAL_FILE, 0);
    if (fh < 0)
        return FALSE;
    n = dos_write((WORD)fh, sizeof(f), &f);
    dos_close((WORD)fh);
    return n == sizeof(f);
}

/* ---- drawing ---- */

static void set_obj(WORD i, WORD next, WORD head, WORD tail, UWORD type,
                    LONG spec, WORD x, WORD y, WORD w, WORD h)
{
    OBJECT *o = &tree[i];

    o->ob_next = next;
    o->ob_head = head;
    o->ob_tail = tail;
    o->ob_type = type;
    o->ob_flags = 0;
    o->ob_state = 0;
    o->ob_spec.index = spec;
    o->ob_x = x;
    o->ob_y = y;
    o->ob_width = w;
    o->ob_height = h;
}

static void build_tree(void)
{
    static const char text[] = "Touch the centre of each cross.";
    WORD ty = scr_h / 2 - 40;           /* between the rows of crosses */

    set_obj(O_ROOT, -1, O_TEXT, O_SQUARE, G_BOX, 0x00000000L,   /* white */
            0, 0, scr_w, scr_h);
    set_obj(O_TEXT, O_COUNT, -1, -1, G_STRING, (LONG)text,
            (scr_w - 8 * (WORD)(sizeof(text) - 1)) / 2, ty, 8 * (sizeof(text) - 1), 16);
    set_obj(O_COUNT, O_HBAR, -1, -1, G_STRING, (LONG)count_text,
            (scr_w - 8 * 6) / 2, ty + 16, 8 * 6, 16);      /* "1 of 9" */
    set_obj(O_HBAR, O_VBAR, -1, -1, G_BOX, 0x00000071L,         /* solid black */
            0, 0, 2 * CROSS + 1, 1);
    set_obj(O_VBAR, O_SQUARE, -1, -1, G_BOX, 0x00000071L,
            0, 0, 1, 2 * CROSS + 1);
    set_obj(O_SQUARE, O_ROOT, -1, -1, G_BOX, 0x00ff1000L,       /* outline */
            0, 0, 7, 7);
    tree[O_SQUARE].ob_flags = LASTOB;
}

static void show_cross(WORD i, BOOL show)
{
    WORD k;

    tree[O_HBAR].ob_x = points[i][0] - CROSS;
    tree[O_HBAR].ob_y = points[i][1];
    tree[O_VBAR].ob_x = points[i][0];
    tree[O_VBAR].ob_y = points[i][1] - CROSS;
    tree[O_SQUARE].ob_x = points[i][0] - 3;
    tree[O_SQUARE].ob_y = points[i][1] - 3;
    for (k = O_HBAR; k <= O_SQUARE; k++)
    {
        if (show)
            tree[k].ob_flags &= ~HIDETREE;
        else
            tree[k].ob_flags |= HIDETREE;
    }
    sprintf(count_text, "%d of %d", i + 1, NPOINTS);
    objc_draw(tree, O_ROOT, MAX_DEPTH, 0, 0, scr_w, scr_h);
}

/* ---- touch input (the mouse is off meanwhile) ---- */

static BOOL touched(short *rx, short *ry)
{
    return tch->get_raw(rx, ry) != 0;
}

static void wait_lifted(void)
{
    short rx, ry;

    while (touched(&rx, &ry))
        evnt_timer(10, 0);
    evnt_timer(200, 0);
}

/* raw position of the next touch, averaged while it lasts (at most 0.5 s) */
static void read_touch(short *raw)
{
    LONG sx = 0, sy = 0;
    short rx, ry;
    WORD n = 0;

    while (!touched(&rx, &ry))
        evnt_timer(10, 0);
    evnt_timer(50, 0);                  /* let it settle */
    while (touched(&rx, &ry) && n < 50)
    {
        sx += rx;
        sy += ry;
        n++;
        evnt_timer(10, 0);
    }
    if (n == 0)
    {
        sx = rx;
        sy = ry;
        n = 1;
    }
    raw[0] = (short)(sx / n);
    raw[1] = (short)(sy / n);
}

/* ---- "Touch calibration..." ---- */

void touch_calibration(void)
{
    struct tch_cal old, cal;
    short raw[NPOINTS][2];
    char alert[128];
    WORD i, choice;
    BOOL ok;
    LONG err;

    if (!touch_present())
    {
        form_alert(1, "[3][No touch screen found.][ OK ]");
        return;
    }
    tch->get_cal(&old);

    for (i = 0; i < NPOINTS; i++)
    {
        points[i][0] = scr_w / 10 + (i % 3) * (scr_w * 4 / 10);
        points[i][1] = scr_h / 10 + (i / 3) * (scr_h * 4 / 10);
    }

    for (;;)
    {
        /* the whole screen is ours until the crosses are done; touches
         * only give raw readings meanwhile, so no BEG_MCTRL is needed */
        wind_update(BEG_UPDATE);
        graf_mouse(M_OFF, NULL);
        tch->set_mouse(0);

        build_tree();
        show_cross(0, FALSE);           /* the empty screen and the text */
        wait_lifted();                  /* the tap on the menu item */
        for (i = 0; i < NPOINTS; i++)
        {
            show_cross(i, TRUE);
            read_touch(raw[i]);
            show_cross(i, FALSE);
            wait_lifted();
        }

        ok = tch_fit(&cal, (const short (*)[2])points,
                     (const short (*)[2])raw, NPOINTS) == 0
             && tch->set_cal(&cal) == 0;
        err = ok ? tch_error(&cal, (const short (*)[2])points,
                             (const short (*)[2])raw, NPOINTS) : 0;

        tch->set_mouse(1);
        graf_mouse(M_ON, NULL);
        wind_update(END_UPDATE);
        form_dial(FMD_FINISH, 0, 0, 0, 0, 0, 0, scr_w, scr_h);  /* windows */
        menu_bar(desk_rs_trees[ADMENU], 1);     /* and the menu bar */

        if (!ok)
        {
            if (form_alert(1, "[3][The touches did not|span the screen.][Again|Cancel]") == 1)
                continue;
            tch->set_cal(&old);
            return;
        }

        sprintf(alert, "[2][Touch calibrated,|mean error %ld.%ld pixels.|"
                       "Save for the next start?][Save|Again|Cancel]",
                err / 10, err % 10);
        choice = form_alert(1, alert);
        if (choice == 2)
            continue;
        if (choice == 3)
            tch->set_cal(&old);
        else if (!save_calibration())
            form_alert(1, "[3][Could not write|" CAL_FILE "][ OK ]");
        return;
    }
}

#endif /* CONF_WITH_TOUCH_CALIBRATION */
