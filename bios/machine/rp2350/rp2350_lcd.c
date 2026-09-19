/*
 * rp2350_lcd.c - 2.8" SPI display (ILI9341) with resistive touch (XPT2046)
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * pTOS draws into an ordinary monochrome framebuffer in SRAM, 320x240,
 * one bit per pixel (9600 bytes), exactly like the ST high resolution
 * but smaller.  The display wants 16 bits per pixel over SPI, so a PIO
 * state machine turns every framebuffer bit into 16 SPI clocks of either
 * black or white, fed by DMA from the framebuffer.  A whole frame goes
 * out about 7 times a second (SPI at 9.4 MHz) without any CPU time; the
 * system timer only starts the next frame when the previous one is done.
 *
 * The touch controller is polled every 10 ms on SPI0 and drives the
 * mouse: position through relative IKBD mouse packets towards the touched
 * point; a tap is a click, holding still presses the button for dragging
 * (see touch_poll()).
 *
 * Wiring: see "SPI display with touch" in
 * docs/superpowers/plans/2026-09-18-rp2350-port.md.
 */

#include "emutos.h"
#include "rp2350.h"
#include "rp2350_lcd.h"
#include "lineavars.h"
#include "bios.h"
#include "ikbd.h"
#include "tosvars.h"
#include "asm.h"
#include "cookie.h"
#include "kprint.h"
#include "touch.h"

#if CONF_WITH_RP2350_LCD

/* pins */
#define LCD_SCK     10
#define LCD_MOSI    11
#define LCD_CS      24
#define LCD_RST     23
#define LCD_DC      25
#define LCD_LED     21
#define T_SCK       6
#define T_MOSI      7
#define T_MISO      20
#define T_CS        8
#define T_IRQ       9

#define FUNC_SPI    RP2350_GPIO_FUNC_SPI
#define FUNC_SIO    RP2350_GPIO_FUNC_SIO
#define FUNC_PIO1   7

#define SIO_IN          RP2350_REG(RP2350_SIO_GPIO_IN)
#define SIO_OUT_SET     RP2350_REG(RP2350_SIO_GPIO_OUT_SET)
#define SIO_OUT_CLR     RP2350_REG(RP2350_SIO_GPIO_OUT_CLR)
#define SIO_OE_SET      RP2350_REG(RP2350_SIO_GPIO_OE_SET)
#define BIT(n)          (1UL << (n))

#define RESETS_CLR      RP2350_REG(RP2350_RESETS_BASE + RP2350_REG_CLR)
#define RESETS_DONE     RP2350_REG(RP2350_RESETS_BASE + 0x08)
#define RESET_DMA       (1UL << 2)
#define RESET_PIO1      (1UL << 12)
#define RESET_SPI0      (1UL << 18)

/* PIO1, state machine 0 */
#define PIO1            0x50300000UL
#define PIO_CTRL        RP2350_REG(PIO1 + 0x000)
#define PIO_FSTAT       RP2350_REG(PIO1 + 0x004)
#define PIO_FDEBUG      RP2350_REG(PIO1 + 0x008)
#define PIO_TXF0_ADDR   (PIO1 + 0x010)
#define PIO_INSTR_MEM(i) RP2350_REG(PIO1 + 0x048 + 4 * (i))
#define PIO_SM0_CLKDIV  RP2350_REG(PIO1 + 0x0c8)
#define PIO_SM0_EXECCTRL RP2350_REG(PIO1 + 0x0cc)
#define PIO_SM0_SHIFTCTRL RP2350_REG(PIO1 + 0x0d0)
#define PIO_SM0_INSTR   RP2350_REG(PIO1 + 0x0d8)
#define PIO_SM0_PINCTRL RP2350_REG(PIO1 + 0x0dc)
#define FSTAT_TXEMPTY0  (1UL << 24)
#define FDEBUG_TXSTALL0 (1UL << 24)

/* DMA channel 0 */
#define DMA             0x50000000UL
#define DMA_READ_ADDR   RP2350_REG(DMA + 0x000)
#define DMA_WRITE_ADDR  RP2350_REG(DMA + 0x004)
#define DMA_TRANS_COUNT RP2350_REG(DMA + 0x008)
#define DMA_CTRL_TRIG   RP2350_REG(DMA + 0x00c)
#define DMA_CTRL_EN     0x1UL
#define DMA_CTRL_HALFWORD (1UL << 2)
#define DMA_CTRL_INCR_READ (1UL << 4)
#define DMA_CTRL_TREQ(n) ((ULONG)(n) << 17)
#define DMA_CTRL_IRQ_QUIET (1UL << 23)
#define DMA_CTRL_BUSY   (1UL << 26)
#define DREQ_PIO1_TX0   8

/* SPI0 (touch) */
#define SPI0            0x40080000UL
#define SSPCR0          RP2350_REG(SPI0 + 0x00)
#define SSPCR1          RP2350_REG(SPI0 + 0x04)
#define SSPDR           RP2350_REG(SPI0 + 0x08)
#define SSPSR           RP2350_REG(SPI0 + 0x0c)
#define SSPCPSR         RP2350_REG(SPI0 + 0x10)
#define SSPSR_TNF       0x02UL
#define SSPSR_RNE       0x04UL

#define WIDTH           320
#define HEIGHT          240
#define FRAME_WORDS     (WIDTH * HEIGHT / 16)  /* UWORDs */

/*
 * PIO program: every framebuffer bit becomes one 16-bit RGB565 pixel.
 * SCK is side-set, MOSI the OUT pin.  A set bit is a black pixel on the
 * ST's monochrome screen, so the bit is inverted: 1 -> 0x0000, 0 -> 0xffff.
 * SPI mode 0: MOSI changes while SCK is low, the display samples on the
 * rising edge.  Two PIO cycles per SPI bit.
 */
static const UWORD lcd_prog[] = {
    0x6021,     /* 0: out x, 1          side 0  ; next pixel (autopull)  */
    0xe04f,     /* 1: set y, 15         side 0                           */
    0xa009,     /* 2: mov pins, ~x      side 0  ; MOSI = colour bit      */
    0x1082      /* 3: jmp y--, 2        side 1  ; SCK high               */
};
#define PROG_LEN    4
#define PIO_CLKDIV  8           /* 18.75 MHz PIO clock: 9.4 MHz SPI, the
                                 * ILI9341 write clock limit is 10 MHz */

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

/* 0: idle, 1: DMA feeding the PIO, 2: DMA done, PIO shifting out the rest */
static UBYTE frame_state;
static UBYTE ticks;
static UBYTE mouse_buttons;

/*
 * Touch as a mouse (polled every 10 ms, see touch_poll()):
 *  - touch and lift without moving: a click; two quick taps: a double click
 *  - touch and move: the pointer follows, no button (menus drop by hover)
 *  - touch and keep still for TOUCH_HOLD: the button goes down; moving then
 *    drags, lifting releases it
 */
#define TOUCH_SETTLE    2       /* polls a touch must last to count */
#define TOUCH_LIFT      3       /* polls without touch that mean "lifted" */
#define TOUCH_HOLD      40      /* polls (400 ms) still for "button down" */
#define TOUCH_STILL     6       /* pixels the finger may wobble when still */

enum { T_IDLE, T_TAP, T_MOVE, T_DRAG };
static UBYTE touch_state;
static UBYTE touch_count;       /* polls touched (settling) or not (lifting) */
static UBYTE still_polls;       /* polls since the finger last moved */
static WORD anchor_x, anchor_y; /* where it was then */

/* ==== display commands, bit-banged while the PIO is not using the pins ==== */

static void pins_to(int func)
{
    rp2350_gpio_set_function(LCD_SCK, func);
    rp2350_gpio_set_function(LCD_MOSI, func);
}

/* about 250 ns at 150 MHz: plenty for the ILI9341 (write cycle >= 100 ns),
 * even through loose wires */
static void bb_wait(void)
{
    int n;

    for (n = 12; n; n--)
        __asm__ volatile ("nop");
}

static void bb_byte(UBYTE b)
{
    int i;

    for (i = 7; i >= 0; i--)
    {
        if (b & (1 << i))
            SIO_OUT_SET = BIT(LCD_MOSI);
        else
            SIO_OUT_CLR = BIT(LCD_MOSI);
        bb_wait();
        SIO_OUT_SET = BIT(LCD_SCK);
        bb_wait();
        SIO_OUT_CLR = BIT(LCD_SCK);
    }
    bb_wait();
}

static void lcd_cmd(UBYTE cmd, const UBYTE *data, int len)
{
    SIO_OUT_CLR = BIT(LCD_DC);
    bb_byte(cmd);
    SIO_OUT_SET = BIT(LCD_DC);
    while (len--)
        bb_byte(*data++);
}

static void lcd_cmd0(UBYTE cmd)
{
    lcd_cmd(cmd, NULL, 0);
}

static void lcd_cmd1(UBYTE cmd, UBYTE d0)
{
    lcd_cmd(cmd, &d0, 1);
}

static void lcd_cmd4(UBYTE cmd, UBYTE d0, UBYTE d1, UBYTE d2, UBYTE d3)
{
    UBYTE d[4];

    d[0] = d0; d[1] = d1; d[2] = d2; d[3] = d3;
    lcd_cmd(cmd, d, 4);
}

static void ms(ULONG n)
{
    armv8m_delay(n * 1000UL);
}

/* ==== frame streaming ======================================================= */

static void frame_start(void)
{
    const UBYTE *fb = v_bas_ad;

    if (!fb)
        return;

    /* RAMWR: the pixels that follow fill the window from its top left */
    pins_to(FUNC_SIO);
    lcd_cmd0(0x2c);
    pins_to(FUNC_PIO1);

    PIO_FDEBUG = FDEBUG_TXSTALL0;
    DMA_READ_ADDR = (ULONG)fb;
    DMA_WRITE_ADDR = PIO_TXF0_ADDR;
    DMA_TRANS_COUNT = FRAME_WORDS;
    DMA_CTRL_TRIG = DMA_CTRL_EN | DMA_CTRL_HALFWORD | DMA_CTRL_INCR_READ
                  | DMA_CTRL_TREQ(DREQ_PIO1_TX0) | DMA_CTRL_IRQ_QUIET;
    frame_state = 1;
}

/*
 * The frame is out when the DMA has fed all of it and the state machine
 * then ran dry.  The stall flag alone does not tell: it is already set
 * while the machine waits for the DMA's first word.  So it is cleared once
 * the DMA is done and the FIFO empty, and the frame is complete when the
 * machine stalls after that.
 */
static void frame_check(void)
{
    if (frame_state == 1 && !(DMA_CTRL_TRIG & DMA_CTRL_BUSY)
        && (PIO_FSTAT & FSTAT_TXEMPTY0))
    {
        PIO_FDEBUG = FDEBUG_TXSTALL0;
        frame_state = 2;
    }
    else if (frame_state == 2 && (PIO_FDEBUG & FDEBUG_TXSTALL0))
        frame_state = 0;
}

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

static WORD map(const struct tch_cal *c, BOOL y, UWORD rx, UWORD ry, WORD max)
{
    long long a = y ? c->ya : c->xa;
    long long b = y ? c->yb : c->xb;
    LONG v = (LONG)((a * rx + b * ry) / c->div) + (y ? c->yoff : c->xoff);

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
    send_mouse(0, 0);
}

static void touch_lifted(void)
{
    if (touch_state == T_TAP)
    {
        set_button(0x02);               /* a tap: press and release */
        set_button(0);
    }
    else if (touch_state == T_DRAG)
        set_button(0);
    touch_state = T_IDLE;
}

static void touch_poll(void)
{
    UWORD rx, ry, a, b;
    WORD x, y;
    int i;

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

    x = map(&cal[cal_idx], FALSE, rx, ry, WIDTH - 1);
    y = map(&cal[cal_idx], TRUE, rx, ry, HEIGHT - 1);
    rp2350_lcd_touch_raw_x = rx;
    rp2350_lcd_touch_raw_y = ry;

    if (touch_state == T_IDLE)
    {
        /* let the contact settle before believing its position */
        if (++touch_count < TOUCH_SETTLE)
            return;
        touch_state = T_TAP;
        anchor_x = x;
        anchor_y = y;
        still_polls = 0;
    }
    touch_count = 0;

    /* the pointer follows the finger, without a button until held still */
    send_mouse(x - linea_vars.GCURX, y - linea_vars.GCURY);

    if (x - anchor_x > TOUCH_STILL || anchor_x - x > TOUCH_STILL
        || y - anchor_y > TOUCH_STILL || anchor_y - y > TOUCH_STILL)
    {
        if (touch_state == T_TAP)
            touch_state = T_MOVE;       /* no longer a tap */
        anchor_x = x;
        anchor_y = y;
        still_polls = 0;
    }
    else if (touch_state != T_DRAG && ++still_polls >= TOUCH_HOLD)
    {
        touch_state = T_DRAG;           /* held still: button down */
        set_button(0x02);
    }
}

UWORD rp2350_lcd_touch_raw_x, rp2350_lcd_touch_raw_y;

/* ==== _TCH cookie: touch interface for programs (include/touch.h) ======== */

static long tch_get_raw(short *rx, short *ry)
{
    *rx = (short)rp2350_lcd_touch_raw_x;
    *ry = (short)rp2350_lcd_touch_raw_y;
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

static const struct tch_api tch_api = {
    TCH_VERSION, sizeof(struct tch_api), WIDTH, HEIGHT,
    tch_get_raw, tch_get_cal, tch_set_cal, tch_set_mouse
};

/* ==== calibration by hand at boot ========================================= */

/*
 * The way out when the calibration is so far off that the calibration
 * program cannot be reached with the pointer: keep a finger on the screen
 * while the machine starts, and it asks for three crosses to be touched,
 * before the desktop comes up.  Runs with interrupts enabled: the timer
 * keeps the display refreshed and the touch polled (with the mouse off).
 */

static const short cal_points[3][2] = {
    { WIDTH / 10, HEIGHT / 10 },
    { WIDTH - WIDTH / 10, HEIGHT / 2 },
    { WIDTH / 2, HEIGHT - HEIGHT / 10 }
};

static void put_pixel(WORD x, WORD y, BOOL on)
{
    /* screen memory: native words, bit 15 the leftmost pixel */
    UWORD *w = (UWORD *)v_bas_ad + y * (WIDTH / 16) + x / 16;
    UWORD m = 0x8000 >> (x & 15);

    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT)
        return;
    if (on)
        *w |= m;
    else
        *w &= ~m;
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
        sx += rp2350_lcd_touch_raw_x;
        sy += rp2350_lcd_touch_raw_y;
        n++;
        wait_ms(10);
    }
    if (n == 0)
    {
        sx = rp2350_lcd_touch_raw_x;
        sy = rp2350_lcd_touch_raw_y;
        n = 1;
    }
    raw[0] = (short)(sx / n);
    raw[1] = (short)(sy / n);
}

void rp2350_lcd_boot_calibration(void)
{
    struct tch_cal c;
    short raw[3][2];
    int i;

    if (SIO_IN & BIT(T_IRQ))            /* not touched: nothing to do */
        return;

    mouse_off = TRUE;
    cprintf("\033E\r\n Touch calibration\r\n\r\n"
            " Lift the finger, then touch the\r\n"
            " centre of each cross as it appears.\r\n");
    wait_ms(30);                        /* the poll sees the finger */
    wait_lifted();

    for (i = 0; i < 3; i++)
    {
        draw_cross(cal_points[i], TRUE);
        read_touch(raw[i]);
        draw_cross(cal_points[i], FALSE);
        wait_lifted();
    }

    if (tch_compute(&c, cal_points, raw) == 0 && tch_set_cal(&c) == 0)
    {
        cal_flags |= TCH_BOOTCAL;
        cprintf("\r\n Done.\r\n");
    }
    else
        cprintf("\r\n Failed, keeping the old calibration.\r\n");
    wait_ms(1000);
    cprintf("\033E");
    mouse_off = FALSE;
}

/* ==== interface ============================================================= */

/* from the 200 Hz system timer (interrupt level) */
void rp2350_lcd_tick(void)
{
    ticks++;

    frame_check();
    if (frame_state == 0 && (ticks & 3) == 0)  /* at most 50 frames/s */
        frame_start();

    if (ticks & 1)                              /* 100 Hz */
        touch_poll();
}

void rp2350_lcd_get_mode(UWORD *planes, UWORD *hz_rez, UWORD *vt_rez)
{
    *planes = 1;
    *hz_rez = WIDTH;
    *vt_rez = HEIGHT;
}

void rp2350_lcd_init(void)
{
    unsigned int i;
    ULONG outs = BIT(LCD_SCK) | BIT(LCD_MOSI) | BIT(LCD_CS) | BIT(LCD_RST)
               | BIT(LCD_DC) | BIT(LCD_LED) | BIT(T_CS);

    RESETS_CLR = RESET_DMA | RESET_PIO1 | RESET_SPI0;
    while ((RESETS_DONE & (RESET_DMA | RESET_PIO1 | RESET_SPI0))
           != (RESET_DMA | RESET_PIO1 | RESET_SPI0))
        ;

    /* control lines: outputs, idle levels */
    SIO_OUT_SET = BIT(LCD_CS) | BIT(LCD_RST) | BIT(LCD_DC) | BIT(T_CS);
    SIO_OUT_CLR = BIT(LCD_SCK) | BIT(LCD_MOSI) | BIT(LCD_LED);
    SIO_OE_SET = outs;
    rp2350_gpio_set_function(LCD_CS, FUNC_SIO);
    rp2350_gpio_set_function(LCD_RST, FUNC_SIO);
    rp2350_gpio_set_function(LCD_DC, FUNC_SIO);
    rp2350_gpio_set_function(LCD_LED, FUNC_SIO);
    rp2350_gpio_set_function(T_CS, FUNC_SIO);
    rp2350_gpio_set_function(T_IRQ, FUNC_SIO);
    rp2350_gpio_pull_up(T_IRQ);
    pins_to(FUNC_SIO);

    /* the display is the only device on its lines: select it for good */
    SIO_OUT_CLR = BIT(LCD_CS);

    /* hardware reset, then the minimal ILI9341 set-up */
    SIO_OUT_CLR = BIT(LCD_RST);
    ms(10);
    SIO_OUT_SET = BIT(LCD_RST);
    ms(120);
    lcd_cmd0(0x01);                     /* software reset */
    ms(150);
    lcd_cmd0(0x11);                     /* sleep out */
    ms(120);
    lcd_cmd1(0x3a, 0x55);               /* 16 bits per pixel */
    lcd_cmd1(0x36, 0x28);               /* landscape (MV), BGR order */
    lcd_cmd4(0x2a, 0, 0, (WIDTH - 1) >> 8, (WIDTH - 1) & 0xff);   /* columns */
    lcd_cmd4(0x2b, 0, 0, (HEIGHT - 1) >> 8, (HEIGHT - 1) & 0xff); /* pages */
    lcd_cmd0(0x29);                     /* display on */
    SIO_OUT_SET = BIT(LCD_LED);         /* backlight */

    /* PIO1 state machine 0 streams the pixels */
    PIO_CTRL = 0;
    for (i = 0; i < PROG_LEN; i++)
        PIO_INSTR_MEM(i) = lcd_prog[i];
    PIO_SM0_CLKDIV = (ULONG)PIO_CLKDIV << 16;
    PIO_SM0_EXECCTRL = ((PROG_LEN - 1UL) << 12);        /* wrap 3 -> 0 */
    /* join TX, autopull every 16 bits, shift left: the DMA writes each
     * framebuffer word to both halves of the FIFO entry, and its bit 15,
     * the leftmost pixel, goes out first (see SCREEN_BYTE() in endian.h) */
    PIO_SM0_SHIFTCTRL = (1UL << 30) | (16UL << 25) | (1UL << 17);
    /* SET base/count only to make both pins outputs */
    PIO_SM0_PINCTRL = (2UL << 26) | ((ULONG)LCD_SCK << 5);
    PIO_SM0_INSTR = 0xe083;                             /* set pindirs, 3 */
    PIO_SM0_PINCTRL = (1UL << 29)                       /* 1 side-set pin */
                    | ((ULONG)LCD_SCK << 10)            /* side-set: SCK */
                    | (1UL << 20) | LCD_MOSI;           /* 1 OUT pin: MOSI */
    PIO_SM0_INSTR = 0x0000;                             /* jmp 0 */
    PIO_CTRL = 1;                                       /* enable SM0 */

    /* SPI0 for the touch controller: 8 bits, mode 0, 150 MHz / 62 = 2.4 MHz */
    SSPCR1 = 0;
    SSPCPSR = 62;
    SSPCR0 = 0x07;
    SSPCR1 = 0x02;
    rp2350_gpio_set_function(T_SCK, FUNC_SPI);
    rp2350_gpio_set_function(T_MOSI, FUNC_SPI);
    rp2350_gpio_set_function(T_MISO, FUNC_SPI);

    /* default calibration: raw Y across, raw X down the screen */
    cal[0].xa = 0;
    cal[0].xb = WIDTH - 1;
    cal[0].xoff = -(LONG)RAW_MIN * (WIDTH - 1) / RAW_SPAN;
    cal[0].ya = HEIGHT - 1;
    cal[0].yb = 0;
    cal[0].yoff = -(LONG)RAW_MIN * (HEIGHT - 1) / RAW_SPAN;
    cal[0].div = RAW_SPAN;
    cal_idx = 0;

    cookie_add(TCH_COOKIE, (ULONG)&tch_api);
}

#endif /* CONF_WITH_RP2350_LCD */
