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
 * out about 25 times a second without any CPU time; the system timer
 * only starts the next frame when the previous one is done.
 *
 * The touch controller is polled every 10 ms on SPI0 and drives the
 * mouse: position through relative IKBD mouse packets towards the touched
 * point, touching as the left button.
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
#define PIO_CLKDIV  2           /* 75 MHz PIO clock: 37.5 MHz SPI */

/* touch calibration: raw XPT2046 readings at the screen edges */
#define TOUCH_X_LEFT    3800
#define TOUCH_X_RIGHT   300
#define TOUCH_Y_TOP     300
#define TOUCH_Y_BOTTOM  3800
#define TOUCH_SWAP_XY   1       /* raw X runs along the screen's y axis */

/* 0: idle, 1: DMA feeding the PIO, 2: DMA done, PIO shifting out the rest */
static UBYTE frame_state;
static UBYTE ticks;
static BOOL pen_down;
static UBYTE mouse_buttons;

/* ==== display commands, bit-banged while the PIO is not using the pins ==== */

static void pins_to(int func)
{
    rp2350_gpio_set_function(LCD_SCK, func);
    rp2350_gpio_set_function(LCD_MOSI, func);
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
        SIO_OUT_SET = BIT(LCD_SCK);
        __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop");
        SIO_OUT_CLR = BIT(LCD_SCK);
    }
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

static WORD scale(UWORD raw, UWORD at0, UWORD atmax, WORD max)
{
    LONG v = ((LONG)raw - at0) * max / ((LONG)atmax - at0);

    return (WORD)(v < 0 ? 0 : v > max ? max : v);
}

static void send_mouse(WORD dx, WORD dy)
{
    SBYTE packet[3];
    WORD sx, sy;

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

static void touch_poll(void)
{
    UWORD rx, ry, a, b;
    WORD x, y;
    int i;

    if (SIO_IN & BIT(T_IRQ))            /* not touched */
    {
        if (pen_down)
        {
            pen_down = FALSE;
            mouse_buttons = 0;
            send_mouse(0, 0);           /* left button up */
        }
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

    /* the pen may have been lifted during the conversion */
    if (SIO_IN & BIT(T_IRQ))
        return;

#if TOUCH_SWAP_XY
    x = scale(ry, TOUCH_Y_TOP, TOUCH_Y_BOTTOM, WIDTH - 1);
    y = scale(rx, TOUCH_X_LEFT, TOUCH_X_RIGHT, HEIGHT - 1);
#else
    x = scale(rx, TOUCH_X_LEFT, TOUCH_X_RIGHT, WIDTH - 1);
    y = scale(ry, TOUCH_Y_TOP, TOUCH_Y_BOTTOM, HEIGHT - 1);
#endif
    rp2350_lcd_touch_raw_x = rx;
    rp2350_lcd_touch_raw_y = ry;

    /* move the pointer there first, then press */
    send_mouse(x - linea_vars.GCURX, y - linea_vars.GCURY);
    if (!pen_down)
    {
        pen_down = TRUE;
        mouse_buttons = 0x02;           /* left button */
        send_mouse(0, 0);
    }
}

UWORD rp2350_lcd_touch_raw_x, rp2350_lcd_touch_raw_y;

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
}

#endif /* CONF_WITH_RP2350_LCD */
