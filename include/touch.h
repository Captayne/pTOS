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
 * (products and sums computed in 64 bits, the division truncating;
 * tch_map() below).  tch_fit() derives it from touched points by a
 * least-squares fit, tch_error() tells how well it matches them.
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

/* screen position for a raw reading (not clipped to the screen) */
static inline void tch_map(const struct tch_cal *cal, long rx, long ry,
                           long *x, long *y)
{
    *x = cal->xoff + (long)(((long long)cal->xa * rx
                             + (long long)cal->xb * ry) / cal->div);
    *y = cal->yoff + (long)(((long long)cal->ya * rx
                             + (long long)cal->yb * ry) / cal->div);
}

#define TCH_FIT_MAX     12      /* points tch_fit() takes at most */

/* a / b rounded to the nearest integer, b > 0 */
static inline long long tch_rdiv(long long a, long long b)
{
    return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}

/*
 * Fit the calibration to n touched points (3 <= n <= TCH_FIT_MAX): the
 * affine map that takes the raw readings raw[i] closest to the screen
 * points scr[i] in the least-squares sense (x at [0], y at [1]).  More
 * points than the three an affine map needs average out the noise of a
 * resistive panel.  Returns 0, or -1 when the points do not span the
 * screen (all on a line).
 *
 * Integer arithmetic only: every sum is taken n times (n * sum(rx * rx) -
 * sum(rx) * sum(rx), ...), which keeps it exact and, for 12-bit readings
 * and at most 12 points, within 64 bits.
 */
static inline long tch_fit(struct tch_cal *cal, const short scr[][2],
                           const short raw[][2], int n)
{
    long long sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    long long kxx, kyy, kxy, d, m;
    long long a[2], b[2], ss[2];
    int i, axis, shift;

    if (n < 3 || n > TCH_FIT_MAX)
        return -1;

    for (i = 0; i < n; i++)
    {
        long long rx = raw[i][0], ry = raw[i][1];

        sx += rx;
        sy += ry;
        sxx += rx * rx;
        syy += ry * ry;
        sxy += rx * ry;
    }
    kxx = n * sxx - sx * sx;
    kyy = n * syy - sy * sy;
    kxy = n * sxy - sx * sy;
    d = kxx * kyy - kxy * kxy;
    if (d <= 0)
        return -1;

    for (axis = 0; axis < 2; axis++)
    {
        long long s = 0, sxs = 0, sys = 0, kxs, kys;

        for (i = 0; i < n; i++)
        {
            s += scr[i][axis];
            sxs += (long long)raw[i][0] * scr[i][axis];
            sys += (long long)raw[i][1] * scr[i][axis];
        }
        kxs = n * sxs - sx * s;
        kys = n * sys - sy * s;
        a[axis] = kxs * kyy - kys * kxy;    /* slope along rx, times d */
        b[axis] = kys * kxx - kxs * kxy;    /* slope along ry, times d */
        ss[axis] = s;
    }

    /* scale the slopes and their divisor down together to 31 bits */
    m = d;
    for (axis = 0; axis < 2; axis++)
    {
        if ((a[axis] < 0 ? -a[axis] : a[axis]) > m)
            m = a[axis] < 0 ? -a[axis] : a[axis];
        if ((b[axis] < 0 ? -b[axis] : b[axis]) > m)
            m = b[axis] < 0 ? -b[axis] : b[axis];
    }
    for (shift = 0; (m >> shift) >= 0x40000000LL; shift++)
        ;
    cal->div = (long)(d >> shift);
    cal->xa = (long)(a[0] >> shift);
    cal->xb = (long)(b[0] >> shift);
    cal->ya = (long)(a[1] >> shift);
    cal->yb = (long)(b[1] >> shift);
    if (cal->div == 0)
        return -1;

    /* the fitted plane passes through the centroid of the points */
    cal->xoff = (long)tch_rdiv(ss[0] * cal->div - (long long)cal->xa * sx
                               - (long long)cal->xb * sy,
                               (long long)n * cal->div);
    cal->yoff = (long)tch_rdiv(ss[1] * cal->div - (long long)cal->ya * sx
                               - (long long)cal->yb * sy,
                               (long long)n * cal->div);

    return 0;
}

/*
 * How well the calibration matches the points: the root mean square
 * distance between where it puts each raw reading and where the point
 * really is, in tenths of a pixel.
 */
static inline long tch_error(const struct tch_cal *cal, const short scr[][2],
                             const short raw[][2], int n)
{
    unsigned long sum = 0, r, bit;
    long x, y;
    int i;

    if (n <= 0)
        return 0;
    for (i = 0; i < n; i++)
    {
        tch_map(cal, raw[i][0], raw[i][1], &x, &y);
        x = (x - scr[i][0]) * 10;
        y = (y - scr[i][1]) * 10;
        sum += (unsigned long)(x * x + y * y);
    }
    sum /= n;

    /* integer square root */
    r = 0;
    for (bit = 1UL << 30; bit > sum; bit >>= 2)
        ;
    for ( ; bit; bit >>= 2)
    {
        if (sum >= r + bit)
        {
            sum -= r + bit;
            r = (r >> 1) + bit;
        }
        else
            r >>= 1;
    }
    return (long)r;
}

#endif /* TOUCH_H */
