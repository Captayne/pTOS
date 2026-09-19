/*
 * touch.h - pTOS touch screen interface: raw readings and calibration
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This header is available under the MIT licence (unlike the rest of
 * pTOS, which is GPL), so that programs of any licence can use it:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions: The above copyright notice and this
 * permission notice shall be included in all copies or substantial
 * portions of the Software.  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT
 * WARRANTY OF ANY KIND.
 *
 * A machine with a touch screen drives the mouse from it and publishes
 * the cookie "_TCH" (Ssystem(S_GETCOOKIE, TCH_COOKIE, 0)), whose value
 * points to a struct tch_api.  Through it a program can read the raw
 * touch position and set the calibration that turns raw readings into
 * screen coordinates -- which is all a calibration tool needs.
 *
 * The calibration is an affine map, so that it also covers a touch panel
 * that is rotated or mirrored against the display:
 *
 *      x = xoff + (xa * rx + xb * ry) / div
 *      y = yoff + (ya * rx + yb * ry) / div
 *
 * (products and sums computed in 64 bits).  tch_compute() below derives
 * it from three touched points.
 *
 * All functions only exchange data with the driver; they can be called
 * from user mode, and from an accessory.
 */

#ifndef TOUCH_H
#define TOUCH_H

#define TCH_COOKIE      0x5f544348L     /* '_TCH' */
#define TCH_VERSION     1

struct tch_cal
{
    long xa, xb, xoff;
    long ya, yb, yoff;
    long div;                   /* 0 is invalid */
};

/* get_cal() result flags */
#define TCH_BOOTCAL     1L      /* calibrated by hand at boot */

struct tch_api
{
    unsigned short version;     /* TCH_VERSION */
    unsigned short size;        /* sizeof(struct tch_api) */
    short width, height;        /* the screen the calibration maps to */

    /* the last raw reading (0..4095 on each axis); 1 while the screen is
     * touched, 0 when not (the reading is then the last one taken) */
    long (*get_raw)(short *rx, short *ry);

    /* the calibration in use; returns TCH_* flags */
    long (*get_cal)(struct tch_cal *cal);

    /* use a new calibration; 0, or -1 when it is invalid */
    long (*set_cal)(const struct tch_cal *cal);

    /* on (1, the default): touches drive the mouse; off (0): they only
     * produce raw readings -- for a calibration tool, whose taps must not
     * click on whatever is below */
    void (*set_mouse)(long on);
};

/*
 * Compute the calibration mapping the raw readings raw[i] to the screen
 * points scr[i] (i = 0..2; x at [0], y at [1]).  The three points must
 * not lie on a line.  Returns 0, or -1 when they do.
 */
static inline long tch_compute(struct tch_cal *cal,
                               const short scr[3][2], const short raw[3][2])
{
    long long rx0 = raw[0][0], ry0 = raw[0][1];
    long long rx1 = raw[1][0], ry1 = raw[1][1];
    long long rx2 = raw[2][0], ry2 = raw[2][1];
    long long d = (rx0 - rx2) * (ry1 - ry2) - (rx1 - rx2) * (ry0 - ry2);
    int axis;

    if (d == 0)
        return -1;

    for (axis = 0; axis < 2; axis++)
    {
        long long s0 = scr[0][axis], s1 = scr[1][axis], s2 = scr[2][axis];
        long long a = (s0 - s2) * (ry1 - ry2) - (s1 - s2) * (ry0 - ry2);
        long long b = (rx0 - rx2) * (s1 - s2) - (s0 - s2) * (rx1 - rx2);
        long long c = ry0 * (rx2 * s1 - rx1 * s2) + ry1 * (rx0 * s2 - rx2 * s0)
                    + ry2 * (rx1 * s0 - rx0 * s1);
        long off = (long)(c / d);

        if (axis == 0)
        {
            cal->xa = (long)a;
            cal->xb = (long)b;
            cal->xoff = off;
        }
        else
        {
            cal->ya = (long)a;
            cal->yb = (long)b;
            cal->yoff = off;
        }
    }
    cal->div = (long)d;

    return 0;
}

#endif /* TOUCH_H */
