/*
 * rp2350_lcd.h - 2.8" SPI display (ILI9341)
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
 * What the framebuffer needs, packed RGB565.  screen.c asks for this
 * before any workstation exists, so it cannot go through the mode
 * descriptor -- and it is no longer a constant, because the size of the
 * glass is no longer decided when this is compiled.  The answer comes
 * from the kept settings (rp2350_nvram_vram_size()).
 */

void rp2350_lcd_init(void);
void rp2350_lcd_tick(void);
void rp2350_lcd_get_mode(UWORD *planes, UWORD *hz_rez, UWORD *vt_rez);
void rp2350_lcd_get_mode_desc(SCREEN_MODE_DESC *desc);
void armv8m_delay(ULONG count);

#endif /* RP2350_LCD_H */
