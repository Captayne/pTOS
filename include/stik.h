/*
 * stik.h - the STiK/STinG transport interface, as GEMbedded provides it
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This header is available under the MIT licence (unlike the rest of
 * pTOS, which is GPL), so that programs of any licence can use it.
 *
 * WHY THIS AND NOT SOMETHING OF OUR OWN
 *
 * STiK (Steve Adam) and its successor STinG (Peter Rottengatter) are how
 * an Atari program has reached a network since the 1990s. Every mail
 * client, every IRC client, every browser on the platform looks for the
 * same cookie and calls the same table. Inventing a cleaner interface
 * would have cost every one of those programs a port; implementing this
 * one costs them a recompile.
 *
 * Binary compatibility is not on offer -- those programs are m68k and
 * this machine is ARM -- but source compatibility is, and that is where
 * nearly all of the work in porting one would otherwise go.
 *
 * HOW A PROGRAM FINDS IT
 *
 *     cookie 'STiK'  ->  DRV_LIST
 *                        .magic    == "STiKmagic"
 *                        .get_dftab("TRANSPORT_TCPIP")  ->  TPL
 *
 * and everything else is reached through the TPL.
 *
 * WHAT IS BEHIND IT HERE
 *
 * An ESP module on UART1, spoken to in AT commands. That shows through
 * in two places, and a program that cares should know:
 *
 *   - One connection at a time. The module can hold several
 *     (AT+CIPMUX=1) but does not today, so TCP_open() past the first
 *     answers E_NOMEM rather than pretending.
 *   - No UDP, no ICMP, no raw sockets. Those entries are present,
 *     because the table's shape is the interface, and they refuse.
 */

#ifndef STIK_H
#define STIK_H

#ifdef __cplusplus
extern "C" {
#endif

#define STIK_COOKIE         0x5354694bL     /* 'STiK' */
#define STIK_DRVR_MAGIC     "STiKmagic"
#define TRANSPORT_DRIVER    "TRANSPORT_TCPIP"

/*
 * The error codes, as STiK defines them. A program tests against these
 * by name and against E_NODATA by value -- "nothing yet" is zero or
 * above, everything below it is a fault.
 */
#define E_NORMAL         0
#define E_OBUFFULL      -1      /* the output buffer is full */
#define E_NODATA        -2      /* nothing has arrived */
#define E_EOF           -3      /* the other end has finished */
#define E_RRESET        -4      /* the other end reset the connection */
#define E_UA            -5      /* unacceptable: no such service there */
#define E_NOMEM        -11
#define E_BADHANDLE    -14
#define E_LOSTCARRIER  -15
#define E_NOHOST       -16
#define E_PARAMETER    -17
#define E_CANTRESOLVE  -22
#define E_NOROUTE      -23
#define E_CNTIMEOUT    -24
#define E_NOCONN       -30
#define E_NOTIMPL      -32

/* TCP_close() timemode */
#define TCP_HALFDUPLEX  (-1)
#define TCP_IMMEDIATE   (0)

/*
 * The transport table.
 *
 * The order is the interface and may not be rearranged: a program
 * compiled against STinG's own header indexes it by position, and an
 * entry moved is an entry called by the wrong name. Everything this
 * machine does not do is still here, answering E_NOTIMPL.
 */
typedef struct tpl
{
    const char *module;         /* "TRANSPORT_TCPIP" */
    const char *author;
    const char *version;        /* "00.00", version:revision */

    void *(*KRmalloc)(long length);
    void  (*KRfree)(void *block);
    long  (*KRgetfree)(short which);
    void *(*KRrealloc)(void *block, long new_length);

    const char *(*get_err_text)(short error_code);
    const char *(*getvstr)(const char *name);
    short (*carrier_detect)(void);

    short (*TCP_open)(unsigned long rem_host, unsigned short rem_port,
                      unsigned short tos, unsigned short buffer_size);
    short (*TCP_close)(short handle, short timemode, short *result);
    short (*TCP_send)(short handle, const void *buffer, short length);
    short (*TCP_wait_state)(short handle, short state, short timeout);
    short (*TCP_ack_wait)(short handle, short timeout);

    short (*UDP_open)(unsigned long rem_host, unsigned short rem_port);
    short (*UDP_close)(short handle);
    short (*UDP_send)(short handle, const void *buffer, short length);

    short (*CNkick)(short handle);
    short (*CNbyte_count)(short handle);
    short (*CNget_char)(short handle);
    void *(*CNget_NDB)(short handle);
    short (*CNget_block)(short handle, void *buffer, short length);

    void  (*housekeep)(void);
    short (*resolve)(const char *domain, char **real, unsigned long *list,
                     short listlen);

    void  (*ser_disable)(void);
    void  (*ser_enable)(void);
    short (*set_flag)(short flag_number);
    void  (*clear_flag)(short flag_number);

    void *(*CNgetinfo)(short handle);
    short (*on_port)(const char *portname);
    void  (*off_port)(const char *portname);
    short (*setvstr)(const char *name, const char *value);
    short (*query_port)(const char *portname);
    short (*CNgets)(short handle, char *buffer, short length, char delim);

    short (*ICMP_send)(unsigned long dest_host, unsigned char type,
                       unsigned char code, const void *data,
                       unsigned short length);
    short (*ICMP_handler)(short (*handler)(void *), short install_code);
    void  (*ICMP_discard)(void *datagram);

    /* STinG, mid-1998 */
    short (*TCP_info)(short handle, void *buffer);
    short (*cntrl_port)(const char *name, unsigned long arg, short code);
    /* STinG 1999.10.01 */
    short (*UDP_info)(short handle, void *buffer);
    /* STinG 2000.06.14, for STiK2 */
    short (*RAW_open)(unsigned long rhost);
    short (*RAW_close)(short handle);
    short (*RAW_out)(short handle, const void *data, short dlen,
                     unsigned long dest_ip);
    short (*CN_setopt)(short handle, short opt_id, const void *optval,
                       short optlen);
    short (*CN_getopt)(short handle, short opt_id, void *optval,
                       short *optlen);
    void  (*CNfree_NDB)(short handle, void *block);

    void *reserved1;
    void *reserved2;
    void *reserved3;
    void *reserved4;
} TPL;

/* What the module header of any driver starts with. */
typedef struct drv_hdr
{
    const char *module;
    const char *author;
    const char *version;
} DRV_HDR;

/* What the cookie points at. */
typedef struct drv_list
{
    char magic[10];                     /* STIK_DRVR_MAGIC */
    DRV_HDR *(*get_dftab)(const char *name);
    short (*ETM_exec)(const char *name);
    void *cfg;                          /* STinG's configuration block */
    void *sting_basepage;
} DRV_LIST;

#ifdef __cplusplus
}
#endif

#endif /* STIK_H */
