/*
 * deskwifi.c - "Connecty..." in the desktop's Options menu
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Connecty is where the machine is told how to reach the world --
 * today the wireless network, its key and the hours from UTC, and
 * whatever else earns a place there later.  Hence the name, and
 * hence one entry rather than one per radio.
 *
 * The machine has no radio: a module on a serial line has one, and a
 * program drives it.  What the machine keeps is the name of the network
 * and its key, so that every program does not have to ask again.  They
 * are entered here, held by the BIOS (bios/wifisettings.c) and written
 * to EMUDESK.INF by "Save desktop".
 *
 * The dialog is built here rather than taken from the resource, the same
 * way the touch calibration builds its crosses: three editable fields,
 * two buttons, and nothing that needs a resource editor.
 *
 * The third field is the hours from UTC.  The machine keeps no rule for
 * when summer time starts -- that is politics, not arithmetic, and it
 * changes -- so whoever sets it says 1 or 2 and is right.
 */

#include "emutos.h"
#include "string.h"
#include "obdefs.h"
#include "aesdefs.h"
#include "aesbind.h"
#include "gemdos.h"
#include "wifi.h"
#include "../bios/wifisettings.h"
#include "deskwifi.h"

#if CONF_WITH_WIFI_SETTINGS

/* what the fields hold while the dialog is open */
static char ssid_text[WIF_SSID_LEN + 1];
static char key_text[WIF_KEY_LEN + 1];
static char utc_text[8];

/* the template and the validation string of an editable field: one
 * character each per position.  "X" accepts anything printable, which is
 * what a network name and a passphrase need. */
static char ssid_tmplt[WIF_SSID_LEN + 1];
static char ssid_valid[WIF_SSID_LEN + 1];
static char key_tmplt[WIF_KEY_LEN + 1];
static char key_valid[WIF_KEY_LEN + 1];
static char utc_tmplt[8];
static char utc_valid[8];

static TEDINFO ted[3];

enum { O_ROOT, O_TITLE, O_NAMELBL, O_NAME, O_KEYLBL, O_KEY,
       O_UTCLBL, O_UTC, O_UTCHINT, O_OK, O_CANCEL, O_NUM };
static OBJECT tree[O_NUM];

#define CW          8                   /* a character, in pixels */
#define CH         16
#define DLG_W      (38 * CW)
#define DLG_H      (11 * CH)

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

/* an editable field: the text itself, a template of underscores, and one
   'X' per position saying "any character is allowed here" */
static void set_field(WORD i, TEDINFO *t, char *text, char *tmplt,
                      char *valid, WORD len, WORD x, WORD y)
{
    WORD k;

    for (k = 0; k < len; k++)
    {
        tmplt[k] = '_';
        valid[k] = 'X';
    }
    tmplt[len] = valid[len] = '\0';

    t->te_ptext = text;
    t->te_ptmplt = tmplt;
    t->te_pvalid = valid;
    t->te_font = IBM;
    t->te_junk1 = 0;
    t->te_just = TE_LEFT;
    t->te_color = 0x1180;               /* black on white, thin border */
    t->te_junk2 = 0;
    t->te_thickness = -1;
    t->te_txtlen = len + 1;
    t->te_tmplen = len + 1;

    tree[i].ob_spec.tedinfo = t;
    tree[i].ob_type = G_FTEXT;
    tree[i].ob_flags = EDITABLE;
    tree[i].ob_x = x;
    tree[i].ob_y = y;
    tree[i].ob_width = len * CW;
    tree[i].ob_height = CH;
}

static void build_tree(void)
{
    set_obj(O_ROOT, -1, O_TITLE, O_CANCEL, G_BOX, 0x00021100L,
            0, 0, DLG_W, DLG_H);
    set_obj(O_TITLE, O_NAMELBL, -1, -1, G_STRING, (LONG)"Connecty",
            CW, CH / 2, 8 * CW, CH);

    set_obj(O_NAMELBL, O_NAME, -1, -1, G_STRING, (LONG)"Name:",
            CW, 2 * CH, 5 * CW, CH);
    set_obj(O_NAME, O_KEYLBL, -1, -1, G_FTEXT, 0L, 0, 0, 0, 0);
    /* the field is as wide as the dialog allows, not as long as an SSID
       may be: it scrolls, the way GEM's editable fields do */
    set_field(O_NAME, &ted[0], ssid_text, ssid_tmplt, ssid_valid,
              28, 7 * CW, 2 * CH);

    set_obj(O_KEYLBL, O_KEY, -1, -1, G_STRING, (LONG)"Key:",
            CW, 4 * CH, 4 * CW, CH);
    set_obj(O_KEY, O_UTCLBL, -1, -1, G_FTEXT, 0L, 0, 0, 0, 0);
    set_field(O_KEY, &ted[1], key_text, key_tmplt, key_valid,
              28, 7 * CW, 4 * CH);

    set_obj(O_UTCLBL, O_UTC, -1, -1, G_STRING, (LONG)"UTC:",
            CW, 6 * CH, 4 * CW, CH);
    set_obj(O_UTC, O_UTCHINT, -1, -1, G_FTEXT, 0L, 0, 0, 0, 0);
    set_field(O_UTC, &ted[2], utc_text, utc_tmplt, utc_valid,
              3, 7 * CW, 6 * CH);
    set_obj(O_UTCHINT, O_OK, -1, -1, G_STRING,
            (LONG)"hours (1 winter, 2 summer)",
            12 * CW, 6 * CH, 26 * CW, CH);

    set_obj(O_OK, O_CANCEL, -1, -1, G_BUTTON, (LONG)"OK",
            CW * 6, 8 * CH + CH / 2, 8 * CW, CH);
    set_obj(O_CANCEL, O_ROOT, -1, -1, G_BUTTON, (LONG)"Cancel",
            CW * 20, 8 * CH + CH / 2, 8 * CW, CH);
    tree[O_OK].ob_flags = SELECTABLE | DEFAULT | EXIT;
    tree[O_CANCEL].ob_flags = SELECTABLE | EXIT | LASTOB;
}

/*
 *  Ask for the network and its key.  What is there already is offered
 *  for editing; OK keeps it, Cancel leaves everything as it was.
 */
void wifi_settings(void)
{
    GRECT d;
    WORD which;

    strlcpy(ssid_text, wifi_get_ssid(), sizeof(ssid_text));
    strlcpy(key_text, wifi_get_key(), sizeof(key_text));
    {
        short h = wifi_get_utc_offset();
        short i = 0;

        if (h < 0)
        {
            utc_text[i++] = '-';
            h = (short)-h;
        }
        if (h >= 10)
            utc_text[i++] = (char)('0' + h / 10);
        utc_text[i++] = (char)('0' + h % 10);
        utc_text[i] = '\0';
    }

    build_tree();

    /* centre it, and let the AES draw it growing out of nothing */
    form_center(tree, &d.g_x, &d.g_y, &d.g_w, &d.g_h);
    form_dial(FMD_START, 0, 0, 0, 0, d.g_x, d.g_y, d.g_w, d.g_h);
    objc_draw(tree, ROOT, MAX_DEPTH, d.g_x, d.g_y, d.g_w, d.g_h);

    which = form_do(tree, O_NAME) & 0x7fff;

    form_dial(FMD_FINISH, 0, 0, 0, 0, d.g_x, d.g_y, d.g_w, d.g_h);

    tree[O_OK].ob_state &= ~SELECTED;
    tree[O_CANCEL].ob_state &= ~SELECTED;

    if (which == O_OK)
    {
        wifi_set(ssid_text, key_text);
        wifi_set_utc_offset_str(utc_text);
    }
}

#endif /* CONF_WITH_WIFI_SETTINGS */
