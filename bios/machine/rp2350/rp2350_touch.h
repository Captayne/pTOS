/*
 * rp2350_touch.h - resistive touch panel (XPT2046) as a pointing device
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * This lived inside rp2350_lcd.c, because the board that started this
 * port has one panel carrying both and one driver was the shortest way
 * to a working screen.  It is not where it belongs:
 *
 *   - A display need not have a touch panel.  An HDMI monitor over HSTX
 *     has none at all, and a parallel RGB panel may carry a capacitive
 *     one on I2C rather than a resistive one on SPI.
 *   - A touch panel is a pointing device, like a mouse, and the two have
 *     to work side by side.  Both feed the same mousevec, and because
 *     this one already turns the absolute position it reads into the
 *     relative movement the vector expects, they simply add up: no
 *     arbitration, no mode switch, nothing to configure.
 *
 * What it still needs from the display is the one thing it cannot do
 * without: the size of the screen, to map a reading onto a pixel.  That
 * comes from the generic screen layer rather than from any particular
 * panel driver, so this depends on "the screen" and not on "that chip".
 */

#ifndef RP2350_TOUCH_H
#define RP2350_TOUCH_H

void rp2350_touch_init(void);               /* pins, SPI0, the _TCH cookie */
void rp2350_touch_tick(void);               /* polled; drives the mouse */
void rp2350_touch_boot_calibration(void);   /* the crosses at start-up */

/* last raw readings, for calibration */
extern UWORD rp2350_touch_raw_x, rp2350_touch_raw_y;
/* mean jump between successive readings while touched, times 16 */
extern UWORD rp2350_touch_noise_x, rp2350_touch_noise_y;
/* taps, double taps, taps turned moves, holds, last double tap gap (polls) */
extern UWORD rp2350_touch_stat[5];

#endif /* RP2350_TOUCH_H */
