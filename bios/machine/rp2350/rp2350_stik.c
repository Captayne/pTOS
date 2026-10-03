/*
 * rp2350_stik.c - the STiK/STinG transport, over an AT module on UART1
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * include/stik.h says what this publishes and why it is that interface
 * and not a tidier one of our own. In short: every Atari program that
 * has ever reached a network looks for the cookie 'STiK' and calls the
 * table behind it, so providing it turns a port into a recompile.
 *
 * Underneath is an ESP module on UART1, spoken to in AT commands, and
 * two of its limits show through the interface rather than being hidden:
 * one connection at a time, and no UDP. Both refuse honestly.
 *
 * WHAT THE AT CONVERSATION COSTS, AND WHAT IT TAUGHT
 *
 * Six faults were found getting this to work the first time, in a
 * version of it that lived in the browser. Every one of them was in the
 * handling of the conversation rather than in the protocol, and five
 * were invisible except in a transcript:
 *
 *   - an empty command is still a command: sending one after the
 *     payload of AT+CIPSEND reaches the module while it is still
 *     sending, and it answers "busy s..." and drops the connection
 *   - "+IPD" and "CLOSED" are not answers to anything. They arrive in
 *     the middle of the reply to whatever is running, and a reader that
 *     hands only expected lines to the caller throws them away
 *   - a line being assembled has to survive the caller's timeout, or a
 *     notification that arrives in two pieces is lost
 *   - the module echoes commands, and "AT+CIPRECVDATA=512" contains the
 *     text its own reply is recognised by
 *   - a fixed buffer that stops filling cannot find a header that has
 *     not arrived yet; it has to slide
 *   - a CLOSED left over from the connection before closes the next one
 *
 * So this does not listen for "+IPD" at all. AT+CIPRECVLEN? asks how
 * much is waiting and gets an answer, like any other command, and the
 * whole class of fault above cannot arise. One more exchange per read is
 * the price, and it is worth paying.
 */

#include "emutos.h"

#if CONF_WITH_RP2350_STIK

#include "string.h"
#include "cookie.h"
#include "delay.h"
#include "stik.h"
#include "uart1.h"
#include "rp2350_uart1.h"
#include "kprint.h"

/*
 * pTOS keeps its string library to what the system itself needs, and
 * these two are not in it. Rather than grow a freestanding kernel's libc
 * for one file, they live here, where their cost is visible.
 */
static const char *find_sub(const char *hay, const char *needle)
{
    size_t n = strlen(needle);

    for (; *hay; hay++)
        if (strncmp(hay, needle, n) == 0)
            return hay;

    return NULL;
}

static void cat_bounded(char *dest, const char *src, size_t room)
{
    size_t len = strlen(dest);

    while (*src && len + 1 < room)
        dest[len++] = *src++;
    dest[len] = '\0';
}

#define ESP_BAUD        115200L
#define THE_HANDLE      1       /* the one connection the module holds */

static BOOL started;            /* the module answered once */
static BOOL conn;               /* a connection is open */
static BOOL peer_gone;          /* ...and the other end has finished */

/* How many of each diagnostic is still worth printing. Reset when a
   connection is opened: these live in pTOS and outlive the program
   being watched, so a ration spent on one page load is gone for the
   next -- which is how an earlier look at this saw nothing at all. */
static int say_hdr, say_blk;

/* ==== talking to the module =========================================== */

static char line[192];          /* the line being assembled, across calls */
static WORD linelen;

/*
 * One line, or -1 when the time is up.
 *
 * What has arrived stays in line[] when it runs out of time. That is the
 * whole point: the caller gives up, the next one carries on where this
 * left off, and a reply split across two reads is still one reply.
 */
static WORD at_line(char *buf, WORD size, LONG msec)
{
    ULONG deadline = monotonic_usec() + (ULONG)msec * 1000UL;

    for (;;)
    {
        char c;

        if (ua1_api.read(&c, 1) != 1)
        {
            if ((LONG)(monotonic_usec() - deadline) >= 0)
                return -1;
            continue;
        }

        if (c == '\n')
        {
            WORD n = (linelen < size - 1) ? linelen : size - 1;

            memcpy(buf, line, n);
            buf[n] = '\0';
            linelen = 0;
            return n;
        }
        if (c == '\r')
            continue;

        if (linelen < (WORD)sizeof(line) - 1)
            line[linelen++] = c;
    }
}

/*
 * Send a command and read until the module has finished answering. Every
 * line before the ending is handed to 'want', which is how a caller
 * picks out the one it needs.
 */
typedef void (*at_want)(const char *line, void *arg);

static BOOL at_cmd(const char *cmd, LONG msec, at_want want, void *arg)
{
    char buf[192];

    if (cmd)
    {
        ua1_api.write(cmd, (LONG)strlen(cmd));
        ua1_api.write("\r\n", 2);
    }

    for (;;)
    {
        if (at_line(buf, (WORD)sizeof(buf), msec) < 0)
            return FALSE;

        if (strcmp(buf, "OK") == 0 || strcmp(buf, "SEND OK") == 0)
            return TRUE;
        if (strcmp(buf, "ERROR") == 0 || strcmp(buf, "FAIL") == 0
         || strcmp(buf, "SEND FAIL") == 0)
            return FALSE;
        if (strcmp(buf, "CLOSED") == 0 && conn)
        {
            peer_gone = TRUE;
            conn = FALSE;
        }

        if (want)
            want(buf, arg);
    }
}

/* Reach the module and put it in the modes the rest of this assumes. */
static BOOL esp_start(void)
{
    if (started)
        return TRUE;

    if (ua1_api.open(ESP_BAUD) < 0)
        return FALSE;
    ua1_api.flush();
    linelen = 0;

    if (!at_cmd("AT", 2000, NULL, NULL))
    {
        KINFO(("stik: no answer to AT\n"));
        return FALSE;
    }

    at_cmd("ATE0", 2000, NULL, NULL);   /* no echo -- see the note above */
    if (!at_cmd("AT+CIPMUX=0", 2000, NULL, NULL))
        return FALSE;
    if (!at_cmd("AT+CIPRECVMODE=1", 2000, NULL, NULL))
        return FALSE;

    started = TRUE;
    KINFO(("stik: module is there\n"));
    return TRUE;
}

/* ==== small helpers =================================================== */

static char *put_num(char *p, ULONG v)
{
    char tmp[12];
    int n = 0;

    if (v == 0)
        *p++ = '0';
    while (v)
    {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n)
        *p++ = tmp[--n];
    *p = '\0';
    return p;
}

static ULONG get_num(const char *p)
{
    ULONG v = 0;

    while (*p >= '0' && *p <= '9')
        v = v * 10 + (ULONG)(*p++ - '0');
    return v;
}

/* ==== the calls a program makes ======================================= */

struct addr_hunt { ULONG addr; };

static void want_domain(const char *l, void *arg)
{
    struct addr_hunt *h = arg;
    const char *p = find_sub(l, "+CIPDOMAIN:");
    ULONG part = 0, out = 0;
    int dots = 0;

    if (!p)
        return;
    p += 11;
    while (*p == '"' || *p == ' ')
        p++;

    for (; *p; p++)
    {
        if (*p >= '0' && *p <= '9')
            part = part * 10 + (ULONG)(*p - '0');
        else if (*p == '.')
        {
            out = (out << 8) | (part & 0xff);
            part = 0;
            dots++;
        }
        else
            break;
    }
    if (dots == 3)
        h->addr = (out << 8) | (part & 0xff);
}

static short stik_resolve(const char *domain, char **real, ULONG *list,
                          short listlen)
{
    struct addr_hunt h;
    char cmd[200];

    if (!domain || !list || listlen < 1)
        return E_PARAMETER;
    if (!esp_start())
        return E_NOHOST;

    h.addr = 0;
    strcpy(cmd, "AT+CIPDOMAIN=\"");
    cat_bounded(cmd, domain, sizeof(cmd) - 2);
    strcat(cmd, "\"");

    if (!at_cmd(cmd, 20000, want_domain, &h) || h.addr == 0)
    {
        KINFO(("stik: resolve %s failed\n", domain));
        return E_CANTRESOLVE;
    }
    KINFO(("stik: %s is %08lx\n", domain, h.addr));

    list[0] = h.addr;
    if (real)
        *real = (char *)domain;         /* no canonical name from AT */
    return 1;                           /* one address found */
}

static short stik_TCP_open(ULONG rem_host, UWORD rem_port, UWORD tos,
                           UWORD buffer_size)
{
    char cmd[80];
    char *p;

    UNUSED(tos);
    UNUSED(buffer_size);

    if (!esp_start())
        return E_NOHOST;
    if (conn)
        return E_NOMEM;                 /* the module holds one at a time */

    /* Start clean: what the last connection left behind is not this
       one's. A stale CLOSED arriving now would shut it at once. */
    ua1_api.flush();
    linelen = 0;
    peer_gone = FALSE;

    p = cmd;
    strcpy(p, "AT+CIPSTART=\"TCP\",\"");
    p += strlen(p);
    p = put_num(p, (rem_host >> 24) & 0xff);  *p++ = '.';
    p = put_num(p, (rem_host >> 16) & 0xff);  *p++ = '.';
    p = put_num(p, (rem_host >> 8) & 0xff);   *p++ = '.';
    p = put_num(p, rem_host & 0xff);
    strcpy(p, "\",");
    p += 2;
    put_num(p, (ULONG)rem_port);

    if (!at_cmd(cmd, 20000, NULL, NULL))
    {
        KINFO(("stik: %s refused\n", cmd));
        return E_CNTIMEOUT;
    }
    KINFO(("stik: connected\n"));

    conn = TRUE;
    say_hdr = say_blk = 0;
    return THE_HANDLE;
}

static short stik_TCP_close(short handle, short timemode, short *result)
{
    UNUSED(timemode);

    if (handle != THE_HANDLE)
        return E_BADHANDLE;

    if (conn)
        at_cmd("AT+CIPCLOSE", 5000, NULL, NULL);
    conn = FALSE;
    peer_gone = FALSE;
    if (result)
        *result = E_NORMAL;
    return E_NORMAL;
}

/*
 * AT+CIPSEND=<len>, then the bytes once the module shows its prompt.
 *
 * The prompt is "> " with no newline, so it is read character by
 * character; a line reader waits for an ending that never comes. And
 * what waits for SEND OK afterwards must send nothing at all -- an empty
 * command is still a command, and it arrives while the module is busy.
 */
static short stik_TCP_send(short handle, const void *buffer, short length)
{
    char cmd[32];
    ULONG deadline;
    BOOL prompt = FALSE;

    if (handle != THE_HANDLE || !conn)
        return E_BADHANDLE;
    if (length <= 0)
        return E_PARAMETER;

    strcpy(cmd, "AT+CIPSEND=");
    put_num(cmd + strlen(cmd), (ULONG)length);

    ua1_api.write(cmd, (LONG)strlen(cmd));
    ua1_api.write("\r\n", 2);

    deadline = monotonic_usec() + 5000000UL;
    while ((LONG)(monotonic_usec() - deadline) < 0)
    {
        char c;

        if (ua1_api.read(&c, 1) == 1 && c == '>')
        {
            prompt = TRUE;
            break;
        }
    }
    if (!prompt)
        return E_OBUFFULL;

    ua1_api.write(buffer, (LONG)length);

    if (!at_cmd(NULL, 20000, NULL, NULL))
        return conn ? E_OBUFFULL : E_LOSTCARRIER;

    return E_NORMAL;
}

struct len_hunt { LONG len; };

static void want_recvlen(const char *l, void *arg)
{
    struct len_hunt *h = arg;
    const char *p = find_sub(l, "+CIPRECVLEN:");

    if (p)
        h->len = (LONG)get_num(p + 12);
}

/*
 * How much is waiting.
 *
 * By asking, not by listening. The module will announce arriving data by
 * itself as "+IPD,<len>", and reading that is the obvious way -- it is
 * also where this went wrong five different ways before, because an
 * unasked-for line turns up in the middle of somebody else's reply.
 * AT+CIPRECVLEN? is a question with an answer.
 */
static short stik_CNbyte_count(short handle)
{
    struct len_hunt h;

    if (handle != THE_HANDLE)
        return E_BADHANDLE;
    if (!conn && !peer_gone)
        return E_NOCONN;

    h.len = 0;
    if (!at_cmd("AT+CIPRECVLEN?", 5000, want_recvlen, &h))
    {
        static BOOL said;

        if (!said)
        {
            said = TRUE;
            KINFO(("stik: AT+CIPRECVLEN? refused -- old firmware?\n"));
        }
        return conn ? E_NODATA : E_EOF;
    }
    {
        static LONG last = -1;

        if (h.len != last)
        {
            last = h.len;
            KINFO(("stik: %ld waiting\n", h.len));
        }
    }

    if (h.len > 0)
        return (short)((h.len > 32767L) ? 32767 : h.len);

    return conn ? E_NODATA : E_EOF;
}

static short stik_CNget_block(short handle, void *buffer, short length)
{
    char cmd[32];
    char hdr[64];
    ULONG deadline;
    WORD i = 0;
    LONG actual = 0, got = 0;
    char *out = buffer;

    if (handle != THE_HANDLE)
        return E_BADHANDLE;
    if (length <= 0)
        return E_PARAMETER;
    if (length > 1460)
        length = 1460;                  /* the module's limit per fetch */

    strcpy(cmd, "AT+CIPRECVDATA=");
    put_num(cmd + strlen(cmd), (ULONG)length);

    ua1_api.write(cmd, (LONG)strlen(cmd));
    ua1_api.write("\r\n", 2);

    /*
     * The header up to its separator, a character at a time: what
     * follows is payload and may hold CR and LF of its own.
     *
     *     +CIPRECVDATA,<len>:<data>     on AT 1.7.x
     *     +CIPRECVDATA:<len>,<data>     on 2.x
     *
     * Found by name, then digits, then one separator -- matching either
     * spelling would work on one firmware and quietly return nothing on
     * the other. The window slides rather than filling up, because what
     * the module volunteered first can be of any length.
     */
    hdr[0] = '\0';
    deadline = monotonic_usec() + 3000000UL;
    for (;;)
    {
        const char *q;
        char c;

        if (ua1_api.read(&c, 1) != 1)
        {
            if ((LONG)(monotonic_usec() - deadline) >= 0)
            {
                /* What it was looking at when it gave up. Without this
                   a header that is not recognised and a module that
                   says nothing look identical from the outside, and
                   both of them simply take ten seconds. */
                if (say_hdr < 3)
                {
                    int k;

                    say_hdr++;
                    for (k = 0; k < i; k++)
                        if (hdr[k] < ' ' || hdr[k] >= 127)
                            hdr[k] = '.';
                    KINFO(("stik: no header, saw [%s]\n", hdr));
                }
                return E_NODATA;
            }
            continue;
        }

        if (i >= (WORD)sizeof(hdr) - 1)
        {
            memmove(hdr, hdr + 1, sizeof(hdr) - 2);
            i--;
        }
        hdr[i++] = c;
        hdr[i] = '\0';

        if (find_sub(hdr, "ERROR"))
            return E_NODATA;

        q = find_sub(hdr, "+CIPRECVDATA");
        if (!q)
            continue;
        q += 12;
        while (*q && (*q < '0' || *q > '9'))
            q++;
        if (!*q)
            continue;                   /* the digits are not all here */
        actual = (LONG)get_num(q);
        while (*q >= '0' && *q <= '9')
            q++;
        if (*q == ':' || *q == ',')
            break;
    }

    if (actual > length)
        actual = length;

    deadline = monotonic_usec() + 3000000UL;
    while (got < actual)
    {
        LONG n = ua1_api.read(out + got, actual - got);

        if (n > 0)
        {
            got += n;
            deadline = monotonic_usec() + 3000000UL;
        }
        else if ((LONG)(monotonic_usec() - deadline) >= 0)
            break;
    }

    at_cmd(NULL, 3000, NULL, NULL);     /* the OK that ends the reply */

    {   /* What was asked for, what the module said, and the start of
           what arrived: enough to tell a short read from a misparsed
           header from data that is simply not what it should be.

           The first few blocks go out in every build, because that is
           what distinguishes a working connection from a broken one and
           it costs a connection three lines.  The rest are KDEBUG: with
           every block shown, a whole reply can be read off the console
           and the exact point its reader stopped at can be seen -- which
           is how the chunk-length bug was found -- but printing a page
           while fetching it would slow down every transfer that works. */
        char peek[65];
        int i;

        for (i = 0; i < (int)sizeof(peek) - 1 && i < got; i++)
            peek[i] = (out[i] >= ' ' && out[i] < 127) ? out[i] : '.';
        peek[i] = '\0';

        if (say_blk < 3)
        {
            say_blk++;
            KINFO(("stik: asked %d, said %ld, got %ld [%s]\n",
                   length, actual, got, peek));
        }
        else
        {
            KDEBUG(("stik: asked %d, said %ld, got %ld [%s]\n",
                    length, actual, got, peek));
        }
    }

    return (short)got;
}

static short stik_CNkick(short handle)
{
    return (handle == THE_HANDLE) ? E_NORMAL : E_BADHANDLE;
}

static short stik_carrier_detect(void)
{
    return esp_start() ? 1 : 0;
}

static const char *stik_get_err_text(short code)
{
    switch (code)
    {
    case E_NORMAL:      return "no error";
    case E_OBUFFULL:    return "output buffer full";
    case E_NODATA:      return "nothing has arrived yet";
    case E_EOF:         return "the other end has finished";
    case E_RRESET:      return "the connection was reset";
    case E_NOMEM:       return "only one connection at a time here";
    case E_BADHANDLE:   return "no such connection";
    case E_NOHOST:      return "no module on UART1";
    case E_CANTRESOLVE: return "that name does not resolve";
    case E_CNTIMEOUT:   return "the connection timed out";
    case E_NOTIMPL:     return "this machine does not do that";
    }
    return "unknown error";
}

/* ==== what this machine does not do =================================== */

/*
 * Present and refusing, rather than absent.
 *
 * The table's shape is the interface: a program indexes it by position,
 * so an entry left out moves every entry after it. These answer
 * E_NOTIMPL, which a caller can act on -- a null pointer it would call.
 */
static short no_short(void) { return E_NOTIMPL; }
static void  no_void(void) { }
static void *no_ptr(void) { return NULL; }
static long  no_long(void) { return 0; }

#define NO_S    no_short
#define NO_V    no_void
#define NO_P    no_ptr
#define NO_L    no_long

/* ==== the tables a program finds ====================================== */

static const TPL tpl = {
    TRANSPORT_DRIVER,
    "GEMbedded",
    "00.01",

    (void *(*)(long))NO_P,              /* KRmalloc */
    (void (*)(void *))NO_V,             /* KRfree */
    (long (*)(short))NO_L,              /* KRgetfree */
    (void *(*)(void *, long))NO_P,      /* KRrealloc */

    stik_get_err_text,
    (const char *(*)(const char *))NO_P, /* getvstr */
    stik_carrier_detect,

    stik_TCP_open,
    stik_TCP_close,
    stik_TCP_send,
    (short (*)(short, short, short))NO_S,   /* TCP_wait_state */
    (short (*)(short, short))NO_S,          /* TCP_ack_wait */

    (short (*)(ULONG, UWORD))NO_S,          /* UDP_open */
    (short (*)(short))NO_S,                 /* UDP_close */
    (short (*)(short, const void *, short))NO_S, /* UDP_send */

    stik_CNkick,
    stik_CNbyte_count,
    (short (*)(short))NO_S,                 /* CNget_char */
    (void *(*)(short))NO_P,                 /* CNget_NDB */
    stik_CNget_block,

    (void (*)(void))NO_V,                   /* housekeep */
    stik_resolve,

    (void (*)(void))NO_V,                   /* ser_disable */
    (void (*)(void))NO_V,                   /* ser_enable */
    (short (*)(short))NO_S,                 /* set_flag */
    (void (*)(short))NO_V,                  /* clear_flag */

    (void *(*)(short))NO_P,                 /* CNgetinfo */
    (short (*)(const char *))NO_S,          /* on_port */
    (void (*)(const char *))NO_V,           /* off_port */
    (short (*)(const char *, const char *))NO_S, /* setvstr */
    (short (*)(const char *))NO_S,          /* query_port */
    (short (*)(short, char *, short, char))NO_S, /* CNgets */

    (short (*)(ULONG, UBYTE, UBYTE, const void *, UWORD))NO_S, /* ICMP_send */
    (short (*)(short (*)(void *), short))NO_S,  /* ICMP_handler */
    (void (*)(void *))NO_V,                 /* ICMP_discard */

    (short (*)(short, void *))NO_S,         /* TCP_info */
    (short (*)(const char *, ULONG, short))NO_S, /* cntrl_port */
    (short (*)(short, void *))NO_S,         /* UDP_info */

    (short (*)(ULONG))NO_S,                 /* RAW_open */
    (short (*)(short))NO_S,                 /* RAW_close */
    (short (*)(short, const void *, short, ULONG))NO_S, /* RAW_out */
    (short (*)(short, short, const void *, short))NO_S, /* CN_setopt */
    (short (*)(short, short, void *, short *))NO_S,     /* CN_getopt */
    (void (*)(short, void *))NO_V,          /* CNfree_NDB */

    NULL, NULL, NULL, NULL
};

static DRV_HDR *stik_get_dftab(const char *name)
{
    if (name && strcmp(name, TRANSPORT_DRIVER) == 0)
        return (DRV_HDR *)&tpl;         /* a TPL starts with a DRV_HDR */

    return NULL;
}

static short stik_etm_exec(const char *name)
{
    UNUSED(name);
    return E_NOTIMPL;                   /* no loadable modules here */
}

static DRV_LIST drivers = {
    STIK_DRVR_MAGIC,
    stik_get_dftab,
    stik_etm_exec,
    NULL,
    NULL
};

void rp2350_stik_add_cookie(void)
{
    cookie_add(STIK_COOKIE, (ULONG)&drivers);
}

#endif /* CONF_WITH_RP2350_STIK */
