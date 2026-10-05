/*
 * rp2350_lcd.c - 2.8" SPI display (ILI9341)
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * pTOS draws into a packed RGB565 framebuffer in SRAM, 320x240, 16 bits
 * per pixel (153600 bytes) -- which is what the display itself wants, so
 * a PIO state machine only has to clock the words out, fed by DMA
 * straight from the framebuffer.  A whole frame goes out about 20 times a
 * second (SPI at 25 MHz) without any CPU time; the system timer only
 * starts the next frame when the previous one is done.
 *
 * Colour costs no SPI time at all: the monochrome mode this replaces
 * expanded every framebuffer bit into 16 SPI clocks of black or white, so
 * the wire already carried a full RGB565 frame.  What the 65536 colours
 * cost is SRAM, 153600 bytes where one bitplane needed 9600.
 *
 * The touch panel on the back of the same glass is not here: it is a
 * pointing device rather than part of the display, and lives in
 * rp2350_touch.c.  All it needs from this side is the size of the
 * screen, and it asks the generic screen layer for that.
 *
 * Wiring: see "SPI display with touch" in
 * docs/superpowers/plans/2026-09-18-rp2350-port.md.
 */

#include "emutos.h"
#include "rp2350.h"
#include "rp2350_lcd.h"
#include "rp2350_nvram.h"
#include "lineavars.h"
#include "tosvars.h"
#include "asm.h"
#include "kprint.h"
#include "screen_mode.h"

#if CONF_WITH_RP2350_LCD

/* pins */
#define LCD_SCK     10
#define LCD_MOSI    11
#define LCD_CS      24
#define LCD_RST     23
#define LCD_DC      25
#define LCD_LED     21

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

/*
 * The glass, as the machine was told it is.  These used to be two
 * #defines, which made the image belong to one piece of glass: a panel
 * of another size meant a compiler.  They are read once, in
 * rp2350_lcd_init(), and used from then on -- a pin number or a pixel
 * count costs nothing at run time, because neither is touched per pixel.
 */
static UWORD lcd_w = CONF_RP2350_LCD_WIDTH;
static UWORD lcd_h = CONF_RP2350_LCD_HEIGHT;

#define WIDTH           lcd_w
#define HEIGHT          lcd_h
#define FRAME_WORDS     ((ULONG)lcd_w * lcd_h)  /* UWORDs: one per pixel */

/*
 * PIO program: shift the framebuffer out a bit at a time, most
 * significant first, which is the order the display reads an RGB565
 * pixel in.  SCK is side-set, MOSI the OUT pin.  SPI mode 0: MOSI changes
 * while SCK is low, the display samples on the rising edge.  Two PIO
 * cycles per SPI bit.
 */
static const UWORD lcd_prog[] = {
    0x6001,     /* 0: out pins, 1       side 0  ; MOSI = next bit        */
    0x1000      /* 1: jmp 0             side 1  ; SCK high               */
};
#define PROG_LEN    2
#define PIO_CLKDIV  3           /* 50 MHz PIO clock: 25 MHz SPI.  The
                                 * ILI9341 is specified for 10 MHz writes,
                                 * but takes much more; 37.5 MHz failed
                                 * (together with too fast commands) */


/* 0: idle, 1: DMA feeding the PIO, 2: DMA done, PIO shifting out the rest */
static UBYTE frame_state;
static UBYTE ticks;

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

/* ==== interface ============================================================= */

/* from the 200 Hz system timer (interrupt level) */
void rp2350_lcd_tick(void)
{
    ticks++;

    frame_check();
    if (frame_state == 0 && (ticks & 3) == 0)  /* at most 50 frames/s */
        frame_start();

}

/*
 * 16 "planes" is how the VDI spells 16 bits per pixel: v_planes only
 * feeds BYTES_LIN (V_REZ_HZ / 8 * v_planes, which is the right pitch for
 * a packed depth too) and the pen counts, and TRUECOLOR_MODE is
 * v_planes > 8.  Which renderer actually draws comes from the descriptor
 * below, never from this.
 */
void rp2350_lcd_get_mode(UWORD *planes, UWORD *hz_rez, UWORD *vt_rez)
{
    *planes = 16;
    *hz_rez = WIDTH;
    *vt_rez = HEIGHT;
}

void rp2350_lcd_get_mode_desc(SCREEN_MODE_DESC *desc)
{
    desc->width = WIDTH;
    desc->height = HEIGHT;
    desc->pitch = WIDTH * sizeof(UWORD);
    desc->bits_per_pixel = 16;
    desc->layout = SCREEN_LAYOUT_PACKED;
    desc->color_model = SCREEN_COLOR_TRUECOLOR;
    desc->pixel_format = SCREEN_PIXEL_RGB565;
    desc->shifter = SCREEN_SHIFTER_NONE;     /* no hardware palette */
}

void rp2350_lcd_init(void)
{
    unsigned int i;
    ULONG outs = BIT(LCD_SCK) | BIT(LCD_MOSI) | BIT(LCD_CS) | BIT(LCD_RST)
               | BIT(LCD_DC) | BIT(LCD_LED);
#if CONF_WITH_RP2350_NVRAM
    const struct rp2350_settings *set = rp2350_nvram_get();

    /*
     * The glass this machine was told it has.  Taken once, here, because
     * screen.c has already sized the framebuffer from the same two
     * numbers and the two must not be able to disagree.
     */
    if (set->scr_w && set->scr_h)
    {
        lcd_w = set->scr_w;
        lcd_h = set->scr_h;
    }
#endif

    RESETS_CLR = RESET_DMA | RESET_PIO1;
    while ((RESETS_DONE & (RESET_DMA | RESET_PIO1))
           != (RESET_DMA | RESET_PIO1))
        ;

    /* control lines: outputs, idle levels */
    SIO_OUT_SET = BIT(LCD_CS) | BIT(LCD_RST) | BIT(LCD_DC);
    SIO_OUT_CLR = BIT(LCD_SCK) | BIT(LCD_MOSI) | BIT(LCD_LED);
    SIO_OE_SET = outs;
    rp2350_gpio_set_function(LCD_CS, FUNC_SIO);
    rp2350_gpio_set_function(LCD_RST, FUNC_SIO);
    rp2350_gpio_set_function(LCD_DC, FUNC_SIO);
    rp2350_gpio_set_function(LCD_LED, FUNC_SIO);
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
    /*
     * Landscape (MV), and bit 3 set: the controller reads the high five
     * bits of a pixel as blue rather than red.
     *
     * That looks backwards next to a framebuffer that holds R5G6B5, and
     * it is -- this panel's glass is wired the other way round, which the
     * bit is there to compensate for. It was clear, and red and blue came
     * out exchanged: the desktop background sits in the framebuffer as
     * 0xf800, which is palette entry 1, which is 0x000000ff in the
     * 0x00BBGGRR form vdi_backend_truecolor.c stores, which is red. It
     * showed blue. Everything from the palette entry to the bit pattern
     * agreed; only the glass did not.
     */
    lcd_cmd1(0x36, 0x28);
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
}

#endif /* CONF_WITH_RP2350_LCD */
