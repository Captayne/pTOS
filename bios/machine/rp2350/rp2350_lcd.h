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

void rp2350_lcd_init(void);
void rp2350_lcd_tick(void);
void rp2350_lcd_get_mode(UWORD *planes, UWORD *hz_rez, UWORD *vt_rez);
void armv8m_delay(ULONG count);

/* last raw touch readings, for calibration */
extern UWORD rp2350_lcd_touch_raw_x, rp2350_lcd_touch_raw_y;

#endif /* RP2350_LCD_H */
