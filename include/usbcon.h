/*
 * usbcon.h - pTOS USB console: raw access for programs
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
 * A machine whose console is a USB serial port publishes the cookie
 * "_UCN" (Ssystem(S_GETCOOKIE, UCN_COOKIE, 0)), whose value points to a
 * struct ucn_api.  Through it a program can take the port over: in raw
 * mode the bytes that arrive no longer become keystrokes but are handed
 * to the program, byte for byte, which is what a file transfer needs.
 *
 * Raw mode belongs to whoever switched it on.  Switch it off before
 * ending, or the console keyboard stays dead.
 */

#ifndef USBCON_H
#define USBCON_H

#define UCN_COOKIE      0x5f55434eL     /* '_UCN' */
#define UCN_VERSION     1

struct ucn_api
{
    unsigned short version;     /* UCN_VERSION */
    unsigned short size;        /* sizeof(struct ucn_api) */

    /* 1: the console hands its bytes to read() instead of the keyboard;
     * 0: back to normal.  Returns the state before the call. */
    long (*set_raw)(long on);

    /* bytes waiting to be read */
    long (*status)(void);

    /* up to len bytes, as many as are there; 0 when there are none */
    long (*read)(void *buf, long len);

    /* all of it, waiting for room on the port */
    long (*write)(const void *buf, long len);
};

#endif /* USBCON_H */
