/*
 * rp2350_lcd.h - 2.8" SPI display (ILI9341) with resistive touch (XPT2046)
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef RP2350_LCD_H
#define RP2350_LCD_H

#include "screen_mode.h"

/*
 * What the framebuffer needs: 320 x 240 packed RGB565.  screen.c asks for
 * this before any workstation exists, so it cannot go through the mode
 * descriptor.
 */
#define RP2350_LCD_VRAM_SIZE    (320UL * 240UL * 2)

void rp2350_lcd_init(void);
void rp2350_lcd_tick(void);
void rp2350_lcd_get_mode(UWORD *planes, UWORD *hz_rez, UWORD *vt_rez);
void rp2350_lcd_get_mode_desc(SCREEN_MODE_DESC *desc);
void rp2350_lcd_boot_calibration(void);
void armv8m_delay(ULONG count);

/* last raw touch readings, for calibration */
extern UWORD rp2350_lcd_touch_raw_x, rp2350_lcd_touch_raw_y;
/* mean jump between successive readings while touched, times 16 */
extern UWORD rp2350_lcd_touch_noise_x, rp2350_lcd_touch_noise_y;
/* taps, double taps, taps turned moves, holds, last double tap gap (polls) */
extern UWORD rp2350_touch_stat[5];

#endif /* RP2350_LCD_H */
