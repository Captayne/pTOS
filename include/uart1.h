/*
 * uart1.h - the second serial port, for programs
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
 * UART0 is the console pTOS prints on while it starts.  UART1, on GPIO 4
 * and 5, is free, and this is the way to it: a radio module, a sensor
 * board, anything that speaks a serial line.
 *
 * WHY THE SYSTEM DRIVES IT AND NOT THE PROGRAM
 *
 * A program can reach the registers itself -- and it will lose
 * characters.  At 115200 baud the hardware holds 2.8 milliseconds of
 * them, and a program that gives the processor away, as it should, gets
 * it back after the next system tick some 20 milliseconds later.  The
 * driver here is fed by the interrupt instead, into a buffer of its own,
 * so a program may take its time and still miss nothing.
 *
 * A program finds it through the cookie "_UA1":
 *
 *     long value;
 *     struct ua1_api *p = 0;
 *     if (Ssystem(S_GETCOOKIE, UA1_COOKIE, (long)&value) == 0)
 *         p = (struct ua1_api *)value;
 *     p->open(115200);
 */

#ifndef UART1_H
#define UART1_H

#define UA1_COOKIE      0x5F554131L     /* '_UA1' */
#define UA1_VERSION     1

#ifdef __cplusplus
extern "C" {
#endif

struct ua1_api {
    unsigned short  version;        /* UA1_VERSION */
    unsigned short  size;           /* sizeof(struct ua1_api) */

    /*
     * Take the port, at this many bits per second: 1200 to 921600, eight
     * bits, no parity.  Whoever opens it owns it until close(); opening
     * it again only changes the speed.  0, or a negative TOS error.
     */
    long (*open)(long baud);
    long (*close)(void);

    long (*status)(void);           /* bytes waiting to be read */
    long (*read)(void *buf, long len);      /* as many as are there */
    long (*write)(const void *buf, long len); /* all of them, waiting for room */

    long (*flush)(void);            /* throw away what has arrived */

    /*
     * Characters that arrived with nowhere to go, since open().  Not an
     * error worth stopping for, but worth knowing: it means somebody was
     * away for too long.
     */
    long (*lost)(void);
};

#ifdef __cplusplus
}
#endif

#endif /* UART1_H */
