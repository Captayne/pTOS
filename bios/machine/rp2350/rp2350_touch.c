/*
 * rp2350_touch.c - resistive touch panel (XPT2046) as a pointing device
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Split out of rp2350_lcd.c, where it lived because the board this port
 * started on carries display and touch on one piece of glass.  See
 * rp2350_touch.h for why that is the wrong place for it.
 *
 * The controller is an XPT2046 on SPI0, polled from the system timer.
 * What it produces is ordinary relative mouse movement, so it sits
 * beside a real mouse rather than instead of one.
 */

#include "emutos.h"
#include "rp2350.h"
#include "rp2350_touch.h"
#include "rp2350_lcd.h"
#include "portab.h"
#include "biosdefs.h"
#include "lineavars.h"
#include "bios.h"
#include "ikbd.h"
#include "tosvars.h"
#include "asm.h"
#include "cookie.h"
#include "kprint.h"
#include "touch.h"
#include "screen_mode.h"

#if CONF_WITH_RP2350_TOUCH

/* pins */
#define T_SCK       6
#define T_MOSI      7
#define T_MISO      20
#define T_CS        8
#define T_IRQ       9

#define FUNC_SPI    RP2350_GPIO_FUNC_SPI
#define FUNC_SIO    RP2350_GPIO_FUNC_SIO

#define SIO_IN          RP2350_REG(RP2350_SIO_GPIO_IN)
#define SIO_OUT_SET     RP2350_REG(RP2350_SIO_GPIO_OUT_SET)
#define SIO_OUT_CLR     RP2350_REG(RP2350_SIO_GPIO_OUT_CLR)
#define SIO_OE_SET      RP2350_REG(RP2350_SIO_GPIO_OE_SET)
#define BIT(n)          (1UL << (n))

#define RESETS_CLR      RP2350_REG(RP2350_RESETS_BASE + RP2350_REG_CLR)
#define RESETS_DONE     RP2350_REG(RP2350_RESETS_BASE + 0x08)
#define RESET_SPI0      (1UL << 18)

/* SPI0 (touch) */
#define SPI0            0x40080000UL
#define SSPCR0          RP2350_REG(SPI0 + 0x00)
#define SSPCR1          RP2350_REG(SPI0 + 0x04)
#define SSPDR           RP2350_REG(SPI0 + 0x08)
#define SSPSR           RP2350_REG(SPI0 + 0x0c)
#define SSPCPSR         RP2350_REG(SPI0 + 0x10)
#define SSPSR_TNF       0x02UL
#define SSPSR_RNE       0x04UL

/*
 * The screen this maps onto.  Read once at init() from the generic
 * screen layer rather than from any particular panel driver: what a
 * reading has to become is a pixel, and which chip puts that pixel on
 * glass is none of this file's business.
 */
static WORD scr_w = 320, scr_h = 240;

/*
 * UWORDs from one scan line to the next.  Not the same as the width:
 * a framebuffer may be wider than what is shown, and put_pixel() below
 * walks the memory, not the picture.
 */
static WORD scr_stride = 320;

/*
 * Touch calibration (see include/touch.h), until a program or the boot
 * time calibration sets a better one: on the TJCTM24028 module, raw X
 * runs down the screen and raw Y across it, from about 300 to 3800.
 * Two copies, so that set_cal() from a program never leaves the timer
 * interrupt a half-written one.
 */
#define RAW_MIN         300
#define RAW_SPAN        3500
static struct tch_cal cal[2];
static volatile UBYTE cal_idx;
static ULONG cal_flags;
static volatile BOOL mouse_off; /* touches only produce raw readings */
static UBYTE mouse_buttons;

/*
 * Touch as a mouse (polled every 10 ms, see touch_poll()):
 *  - touch and lift without moving: a click; two quick taps: a double click
 *  - touch and move: the pointer follows, no button (menus drop by hover);
 *    it jumps to where the finger lands, but then only follows once the
 *    finger has moved more than TOUCH_STILL -- the AES cancels its wait
 *    for a second click as soon as the mouse moves
 *  - touch and keep still for TOUCH_HOLD: the button goes down; moving then
 *    drags, lifting releases it
 *  - a tap soon after a tap, near it: the button goes down at once, where
 *    the first tap was -- with a tap clicking only when lifted, the second
 *    click of a double click would otherwise come too late for the AES
 */
#define TOUCH_SETTLE    2       /* polls a touch must last to count */
#define TOUCH_LIFT      3       /* polls without touch that mean "lifted" */
#define TOUCH_HOLD      40      /* polls (400 ms) still for "button down" */
#define TOUCH_STILL     10      /* pixels the finger may wobble when still */
#define TOUCH_DCLICK    40      /* polls (400 ms) from a tap to a second one */
#define TOUCH_DNEAR     24      /* pixels between them */
/*
 * How long a tap holds the button down.  A finger lifting is one event,
 * but a click is two, and a program is entitled to look at the button
 * between them: GEM draws a widget pressed while it is down and acts on
 * it when it comes up, and graf_mkstate() asks for the state directly.
 * Pressing and releasing in the same instant leaves nothing to find --
 * the widget flickers, and whether the click counts at all depends on
 * where in its event loop the program happened to be.
 *
 * 80 ms is in the middle of a real click (50-150 ms) and still well
 * under TOUCH_DCLICK, so a deliberate double tap is not swallowed.
 */
#define TOUCH_CLICK     8       /* polls (80 ms) a tap holds the button */

enum { T_IDLE, T_TAP, T_MOVE, T_DRAG };
static UBYTE touch_state;
static UBYTE touch_count;       /* polls touched (settling) or not (lifting) */
static UBYTE click_polls;       /* polls left of a tap's button press */
static UBYTE still_polls;       /* polls since the finger last moved */
static WORD anchor_x, anchor_y; /* where it was then */
static ULONG polls;             /* touch_poll() calls */
static ULONG tap_polls;         /* when the last tap ended */
static WORD tap_x, tap_y;       /* and where */
static BOOL tap_valid;
static BOOL follow;             /* the pointer follows the finger */

/* bring-up statistics, reported by rp2350_monitor.c */
UWORD rp2350_touch_stat[5];     /* taps, double taps, moves, holds, gap */

/* ==== touch ================================================================= */

static UBYTE spi0_xfer(UBYTE out)
{
    while (!(SSPSR & SSPSR_TNF))
        ;
    SSPDR = out;
    while (!(SSPSR & SSPSR_RNE))
        ;
    return (UBYTE)SSPDR;
}

/* one 12-bit conversion; cmd selects the channel, PENIRQ stays enabled */
static UWORD touch_read(UBYTE cmd)
{
    UWORD v;

    (void)spi0_xfer(cmd);
    v = (UWORD)spi0_xfer(0) << 8;
    v |= spi0_xfer(0);
    return v >> 3;
}

static WORD clip(LONG v, WORD max)
{
    return (WORD)(v < 0 ? 0 : v > max ? max : v);
}

static void send_mouse(WORD dx, WORD dy)
{
    SBYTE packet[3];
    WORD sx, sy;

    if (mouse_off)
        return;
    do
    {
        sx = (dx > 127) ? 127 : (dx < -128) ? -128 : dx;
        sy = (dy > 127) ? 127 : (dy < -128) ? -128 : dy;
        packet[0] = (SBYTE)(0xf8 | mouse_buttons);
        packet[1] = (SBYTE)sx;
        packet[2] = (SBYTE)sy;
        call_mousevec(packet);
        dx -= sx;
        dy -= sy;
    } while (dx || dy);
}

static void set_button(UBYTE buttons)
{
    mouse_buttons = buttons;
    click_polls = 0;            /* whoever presses or releases now owns
                                   the button; no stale timer may undo it */
    send_mouse(0, 0);
}

static void touch_lifted(void)
{
    if (touch_state == T_TAP)
    {
        /* A tap presses here and releases TOUCH_CLICK polls later, in
           touch_poll(): a click a program can actually observe. */
        set_button(0x02);
        click_polls = TOUCH_CLICK;
        rp2350_touch_stat[0]++;
        tap_valid = TRUE;
        tap_polls = polls;
        tap_x = anchor_x;
        tap_y = anchor_y;
    }
    else if (touch_state == T_DRAG)
        set_button(0);
    touch_state = T_IDLE;
}

/* noise: running mean of the jump between successive raw readings while
 * touched, times 16 (bring-up aid, see rp2350_monitor.c) */
UWORD rp2350_touch_noise_x, rp2350_touch_noise_y;
static UWORD prev_rx, prev_ry;

static void track_noise(UWORD rx, UWORD ry)
{
    UWORD dx = rx > prev_rx ? rx - prev_rx : prev_rx - rx;
    UWORD dy = ry > prev_ry ? ry - prev_ry : prev_ry - ry;

    if (touch_state == T_TAP || touch_state == T_DRAG)     /* finger still */
    {
        rp2350_touch_noise_x += dx - (rp2350_touch_noise_x >> 4);
        rp2350_touch_noise_y += dy - (rp2350_touch_noise_y >> 4);
    }
    prev_rx = rx;
    prev_ry = ry;
}

static void touch_poll(void)
{
    UWORD rx, ry, a, b;
    WORD x, y;
    int i;

    polls++;

    /* the release that ends a tap's click, whether or not the panel is
     * being touched again by now (see touch_lifted()) */
    if (click_polls && --click_polls == 0)
        set_button(0);

    /* mouse switched off by a program in the middle of a drag: the
     * button must not stay down */
    if (mouse_off && mouse_buttons)
    {
        mouse_off = FALSE;
        set_button(0);
        mouse_off = TRUE;
    }

    if (SIO_IN & BIT(T_IRQ))            /* not touched */
    {
        if (touch_state == T_IDLE)
            touch_count = 0;            /* a touch too short to count */
        else if (++touch_count >= TOUCH_LIFT)
            touch_lifted();
        return;
    }

    /* average four samples of each axis */
    a = b = 0;
    SIO_OUT_CLR = BIT(T_CS);
    for (i = 0; i < 4; i++)
    {
        a += touch_read(0xd0);          /* X+ */
        b += touch_read(0x90);          /* Y+ */
    }
    SIO_OUT_SET = BIT(T_CS);
    rx = a / 4;
    ry = b / 4;

    /* the pen may have been lifted during the conversion, and a light
     * touch gives readings at the ends of the range: no information */
    if ((SIO_IN & BIT(T_IRQ)) || rx < 50 || rx > 4045 || ry < 50 || ry > 4045)
        return;

    {
        LONG sx, sy;

        tch_map(&cal[cal_idx], rx, ry, &sx, &sy);
        x = clip(sx, scr_w - 1);
        y = clip(sy, scr_h - 1);
    }
    rp2350_touch_raw_x = rx;
    rp2350_touch_raw_y = ry;
    track_noise(rx, ry);

    if (touch_state == T_IDLE)
    {
        /* let the contact settle before believing its position */
        if (++touch_count < TOUCH_SETTLE)
            return;
        touch_count = 0;
        touch_state = T_TAP;
        still_polls = 0;
        follow = FALSE;

        if (tap_valid && polls - tap_polls <= TOUCH_DCLICK
            && x - tap_x <= TOUCH_DNEAR && tap_x - x <= TOUCH_DNEAR
            && y - tap_y <= TOUCH_DNEAR && tap_y - y <= TOUCH_DNEAR)
        {
            /* second tap of a double click: press at once, and without
             * moving the pointer -- the AES ends its wait for a second
             * click as soon as the mouse moves (mchange()) */
            rp2350_touch_stat[1]++;
            rp2350_touch_stat[4] = (UWORD)(polls - tap_polls);
            tap_valid = FALSE;
            anchor_x = x;
            anchor_y = y;
            touch_state = T_DRAG;
            set_button(0x02);
            return;
        }

        /* a new touch: the pointer jumps there, then stays put until the
         * finger really moves (see above) */
        anchor_x = x;
        anchor_y = y;
        send_mouse(x - linea_vars.GCURX, y - linea_vars.GCURY);
        return;
    }
    touch_count = 0;

    if (!follow && (x - anchor_x > TOUCH_STILL || anchor_x - x > TOUCH_STILL
                    || y - anchor_y > TOUCH_STILL || anchor_y - y > TOUCH_STILL))
    {
        follow = TRUE;                  /* the finger moves: follow it */
        if (touch_state == T_TAP)
        {
            touch_state = T_MOVE;       /* no longer a tap */
            rp2350_touch_stat[2]++;
        }
    }

    if (follow)
    {
        send_mouse(x - linea_vars.GCURX, y - linea_vars.GCURY);
        if (touch_state == T_MOVE)
        {
            /* holding still after moving presses the button as well */
            if (x - anchor_x > TOUCH_STILL || anchor_x - x > TOUCH_STILL
                || y - anchor_y > TOUCH_STILL || anchor_y - y > TOUCH_STILL)
            {
                anchor_x = x;
                anchor_y = y;
                still_polls = 0;
            }
            else if (++still_polls >= TOUCH_HOLD)
            {
                touch_state = T_DRAG;
                rp2350_touch_stat[3]++;
                set_button(0x02);
            }
        }
    }
    else if (touch_state == T_TAP && ++still_polls >= TOUCH_HOLD)
    {
        touch_state = T_DRAG;           /* held still: button down */
        rp2350_touch_stat[3]++;
        set_button(0x02);
    }
}

UWORD rp2350_touch_raw_x, rp2350_touch_raw_y;

/* ==== _TCH cookie: touch interface for programs (include/touch.h) ======== */

static long tch_get_raw(short *rx, short *ry)
{
    *rx = (short)rp2350_touch_raw_x;
    *ry = (short)rp2350_touch_raw_y;
    return touch_state != T_IDLE;
}

static long tch_get_cal(struct tch_cal *c)
{
    *c = cal[cal_idx];
    return (long)cal_flags;
}

static long tch_set_cal(const struct tch_cal *c)
{
    UBYTE next = cal_idx ^ 1;

    if (c->div == 0)
        return -1;
    cal[next] = *c;
    cal_idx = next;                 /* the interrupt now uses the new one */
    return 0;
}

static void tch_set_mouse(long on)
{
    mouse_off = !on;
}

static struct tch_api tch_api = {
    TCH_VERSION, sizeof(struct tch_api), 0, 0,   /* size filled by init() */
    tch_get_raw, tch_get_cal, tch_set_cal, tch_set_mouse
};

/* ==== calibration by hand at boot ========================================= */

/*
 * The way out when the calibration is so far off that the calibration
 * program cannot be reached with the pointer: keep a finger on the screen
 * while the machine starts, and it asks for nine crosses to be touched,
 * before the desktop comes up.  Runs with interrupts enabled: the timer
 * keeps the display refreshed and the touch polled (with the mouse off).
 */

/* a 3 x 3 grid at 10 %, 50 % and 90 % of the screen, fitted by least
 * squares (tch_fit()) */
#define CAL_POINTS      9
static short cal_points[CAL_POINTS][2];

/*
 * Tenths rather than pixels, because the screen size is not known until
 * init().  For 320 x 240 this reproduces what used to be written out
 * here exactly: a tenth of 320 is 32, nine tenths is 288.
 */
static void cal_points_init(void)
{
    static const UBYTE tenths[CAL_POINTS][2] = {
        { 1, 1 }, { 5, 1 }, { 9, 1 },
        { 1, 5 }, { 5, 5 }, { 9, 5 },
        { 1, 9 }, { 5, 9 }, { 9, 9 }
    };
    int i;

    for (i = 0; i < CAL_POINTS; i++)
    {
        cal_points[i][0] = (short)((LONG)scr_w * tenths[i][0] / 10);
        cal_points[i][1] = (short)((LONG)scr_h * tenths[i][1] / 10);
    }
}

static void put_pixel(WORD x, WORD y, BOOL on)
{
    if (x < 0 || x >= scr_w || y < 0 || y >= scr_h)
        return;

    /* the crosses are drawn before any workstation is open, so straight
     * into the framebuffer rather than through the VDI's palette */
    *((UWORD *)v_bas_ad + (ULONG)y * scr_stride + x) = on ? 0x0000 : 0xffff;
}

static void draw_cross(const short *p, BOOL on)
{
    WORD i;

    for (i = -12; i <= 12; i++)
    {
        put_pixel(p[0] + i, p[1], on);
        put_pixel(p[0], p[1] + i, on);
        if (i >= -2 && i <= 2)          /* a hollow centre square */
        {
            put_pixel(p[0] + i, p[1] - 3, on);
            put_pixel(p[0] + i, p[1] + 3, on);
            put_pixel(p[0] - 3, p[1] + i, on);
            put_pixel(p[0] + 3, p[1] + i, on);
        }
    }
}

static void wait_ms(ULONG n)
{
    armv8m_delay(n * 1000UL);
}

static void wait_lifted(void)
{
    while (touch_state != T_IDLE)
        wait_ms(10);
    wait_ms(200);
}

/* raw position of the next touch, averaged while it lasts (at most 0.5 s) */
static void read_touch(short *raw)
{
    LONG sx = 0, sy = 0;
    int n = 0;

    while (touch_state == T_IDLE)
        wait_ms(10);
    wait_ms(50);                        /* let it settle */
    while (touch_state != T_IDLE && n < 50)
    {
        sx += rp2350_touch_raw_x;
        sy += rp2350_touch_raw_y;
        n++;
        wait_ms(10);
    }
    if (n == 0)
    {
        sx = rp2350_touch_raw_x;
        sy = rp2350_touch_raw_y;
        n = 1;
    }
    raw[0] = (short)(sx / n);
    raw[1] = (short)(sy / n);
}

void rp2350_touch_boot_calibration(void)
{
    struct tch_cal c;
    short raw[CAL_POINTS][2];
    int i;

    if (SIO_IN & BIT(T_IRQ))            /* not touched: nothing to do */
        return;

    mouse_off = TRUE;
    cprintf("\033E\r\n Touch calibration\r\n\r\n"
            " Lift the finger, then touch the\r\n"
            " centre of each cross as it appears.\r\n");
    wait_ms(30);                        /* the poll sees the finger */
    wait_lifted();

    for (i = 0; i < CAL_POINTS; i++)
    {
        draw_cross(cal_points[i], TRUE);
        read_touch(raw[i]);
        draw_cross(cal_points[i], FALSE);
        wait_lifted();
    }

    if (tch_fit(&c, cal_points, (const short (*)[2])raw, CAL_POINTS) == 0
        && tch_set_cal(&c) == 0)
    {
        LONG err = tch_error(&c, cal_points, (const short (*)[2])raw, CAL_POINTS);

        cal_flags |= TCH_BOOTCAL;
        cprintf("\r\n Done, mean error %ld.%ld pixels.\r\n", err / 10, err % 10);
    }
    else
        cprintf("\r\n Failed, keeping the old calibration.\r\n");
    wait_ms(2000);
    cprintf("\033E");
    mouse_off = FALSE;
}


/* ==== interface ============================================================ */

/* from the 200 Hz system timer (interrupt level), halved to 100 Hz */
void rp2350_touch_tick(void)
{
    static UBYTE tick_count;

    if (++tick_count & 1)
        touch_poll();
}

void rp2350_touch_init(void)
{
    SCREEN_MODE_DESC desc;

    /* the size a reading is mapped onto; see scr_w above */
    screen_get_current_mode_desc(&desc);
    if (desc.width > 0 && desc.height > 0)
    {
        scr_w = (WORD)desc.width;
        scr_h = (WORD)desc.height;
        scr_stride = (WORD)(desc.pitch / sizeof(UWORD));
    }

    /* SPI0 alone: the display takes DMA and PIO1 out of reset itself */
    RESETS_CLR = RESET_SPI0;
    while ((RESETS_DONE & RESET_SPI0) != RESET_SPI0)
        ;

    SIO_OUT_SET = BIT(T_CS);
    SIO_OE_SET  = BIT(T_CS);
    rp2350_gpio_set_function(T_CS, FUNC_SIO);
    rp2350_gpio_set_function(T_IRQ, FUNC_SIO);
    rp2350_gpio_pull_up(T_IRQ);

    /* 8 bits, mode 0, 150 MHz / 62 = 2.4 MHz */
    SSPCR1 = 0;
    SSPCPSR = 62;
    SSPCR0 = 0x07;
    SSPCR1 = 0x02;
    rp2350_gpio_set_function(T_SCK, FUNC_SPI);
    rp2350_gpio_set_function(T_MOSI, FUNC_SPI);
    rp2350_gpio_set_function(T_MISO, FUNC_SPI);

    tch_api.width = scr_w;
    tch_api.height = scr_h;
    cal_points_init();

    /* default calibration: raw Y across, raw X down the screen */
    cal[0].xa = 0;
    cal[0].xb = scr_w - 1;
    cal[0].xoff = -(LONG)RAW_MIN * (scr_w - 1) / RAW_SPAN;
    cal[0].ya = scr_h - 1;
    cal[0].yb = 0;
    cal[0].yoff = -(LONG)RAW_MIN * (scr_h - 1) / RAW_SPAN;
    cal[0].div = RAW_SPAN;
    cal_idx = 0;

    cookie_add(TCH_COOKIE, (ULONG)&tch_api);
}

#endif /* CONF_WITH_RP2350_TOUCH */
