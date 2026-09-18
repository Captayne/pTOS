/*
 * rp2350_usbcon.c - console over USB (CDC ACM) on the RP2350 USB controller
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * The Waveshare RP2350-PiZero has no USB-to-serial bridge, so the native
 * USB port (the USB-C socket that also powers the board) presents itself
 * as a CDC ACM serial port.  On Windows 10 and later, and on Linux and
 * macOS, the standard class driver picks it up without any driver
 * installation.
 *
 * This is a small, self-contained device-mode driver for the controller
 * described in chapter 12.7 ("USB") of the RP2350 datasheet: endpoint 0
 * for enumeration, endpoint 1 IN (interrupt, CDC notifications, never
 * used), endpoint 2 OUT/IN (bulk data).  All data moves through the
 * controller's 4 KB dual-port RAM.
 *
 * Output is buffered in a ring from the very first character, long before
 * USB is up, so the boot messages are waiting when a terminal program
 * opens the port.  While no terminal has the port open (DTR clear),
 * characters that do not fit into the ring are dropped instead of
 * blocking the system.
 *
 * Like the Raspberry Pi pico-sdk's USB stdio, setting the port to 1200
 * baud reboots the chip into its BOOTSEL mode, so that a new image can be
 * copied onto it without touching the BOOT button.
 *
 * The VID/PID is the pid.codes test pair 1209:0001, which is meant for
 * exactly this kind of development use; a real PID will be needed before
 * distributing images.
 */

#include "emutos.h"
#include "rp2350.h"
#include "rp2350_int.h"
#include "rp2350_usbcon.h"
#include "ikbd.h"
#include "asm.h"
#include "string.h"

/* Controller registers */
#define USB_REGS            0x50110000UL
#define USB_REG(off)        RP2350_REG(USB_REGS + (off))
#define USB_REG_SET(off)    RP2350_REG(USB_REGS + RP2350_REG_SET + (off))
#define USB_REG_CLR(off)    RP2350_REG(USB_REGS + RP2350_REG_CLR + (off))

#define ADDR_ENDP           0x00
#define MAIN_CTRL           0x40
#define SIE_CTRL            0x4c
#define SIE_STATUS          0x50
#define BUFF_STATUS         0x58
#define EP_STALL_ARM        0x68
#define USB_MUXING          0x74
#define USB_PWR             0x78
#define INTE                0x90
#define INTS                0x98

#define MAIN_CTRL_CONTROLLER_EN     0x00000001UL
#define MAIN_CTRL_PHY_ISO           0x00000004UL
#define SIE_CTRL_EP0_INT_1BUF       0x20000000UL
#define SIE_CTRL_PULLUP_EN          0x00010000UL
#define SIE_STATUS_BUS_RESET        0x00080000UL
#define SIE_STATUS_SETUP_REC        0x00020000UL
#define USB_MUXING_SOFTCON          0x00000008UL
#define USB_MUXING_TO_PHY           0x00000001UL
#define USB_PWR_VBUS_DETECT_OVERRIDE_EN 0x00000008UL
#define USB_PWR_VBUS_DETECT         0x00000004UL
#define INT_SETUP_REQ               0x00010000UL
#define INT_BUS_RESET               0x00001000UL
#define INT_BUFF_STATUS             0x00000010UL

/* Dual-port RAM */
#define DPRAM               0x50100000UL
#define DPRAM_PTR(off)      ((volatile UBYTE *)(DPRAM + (off)))
#define EP_CTRL(off)        RP2350_REG(DPRAM + (off))
#define BUF_CTRL(off)       RP2350_REG(DPRAM + (off))

#define EP1_IN_CTRL         0x08
#define EP2_IN_CTRL         0x10
#define EP2_OUT_CTRL        0x14
#define EP0_IN_BUF_CTRL     0x80
#define EP0_OUT_BUF_CTRL    0x84
#define EP1_IN_BUF_CTRL     0x88
#define EP2_IN_BUF_CTRL     0x90
#define EP2_OUT_BUF_CTRL    0x94

#define EP0_BUF             0x100
#define EP1_IN_BUF          0x180
#define EP2_IN_BUF          0x1c0
#define EP2_OUT_BUF         0x200

#define EP_CTRL_ENABLE          0x80000000UL
#define EP_CTRL_INT_PER_BUFF    0x20000000UL
#define EP_CTRL_TYPE_BULK       (2UL << 26)
#define EP_CTRL_TYPE_INTERRUPT  (3UL << 26)

#define BUF_FULL        0x00008000UL
#define BUF_LAST        0x00004000UL
#define BUF_DATA1       0x00002000UL
#define BUF_STALL       0x00000800UL
#define BUF_AVAIL       0x00000400UL
#define BUF_LEN_MASK    0x000003ffUL

/* BUFF_STATUS / EP_STALL_ARM bits */
#define EP0_IN_BIT      0x01UL
#define EP0_OUT_BIT     0x02UL
#define EP2_IN_BIT      0x10UL
#define EP2_OUT_BIT     0x20UL

#define EP0_SIZE        64
#define BULK_SIZE       64

/* Standard and CDC requests */
#define REQ_GET_STATUS          0x00
#define REQ_SET_ADDRESS         0x05
#define REQ_GET_DESCRIPTOR      0x06
#define REQ_GET_CONFIGURATION   0x08
#define REQ_SET_CONFIGURATION   0x09
#define CDC_SET_LINE_CODING     0x20
#define CDC_GET_LINE_CODING     0x21
#define CDC_SET_CONTROL_LINE_STATE 0x22
#define CDC_SEND_BREAK          0x23

#define DESC_DEVICE     1
#define DESC_CONFIG     2
#define DESC_STRING     3

static const UBYTE device_desc[18] = {
    18, DESC_DEVICE,
    0x00, 0x02,             /* USB 2.0 */
    0xef, 0x02, 0x01,       /* miscellaneous: interface association */
    EP0_SIZE,
    0x09, 0x12,             /* VID 0x1209 (pid.codes) */
    0x01, 0x00,             /* PID 0x0001 (test) */
    0x00, 0x01,             /* device release 1.00 */
    1, 2, 3,                /* manufacturer, product, serial number */
    1                       /* one configuration */
};

#define CONFIG_DESC_LEN 75
static const UBYTE config_desc[CONFIG_DESC_LEN] = {
    9, DESC_CONFIG, CONFIG_DESC_LEN, 0, 2, 1, 0, 0x80, 250,
    /* interface association: interfaces 0 and 1 form one CDC ACM function */
    8, 0x0b, 0, 2, 0x02, 0x02, 0x00, 0,
    /* interface 0: communications class, ACM, one notification endpoint */
    9, 4, 0, 0, 1, 0x02, 0x02, 0x00, 0,
    5, 0x24, 0x00, 0x10, 0x01,          /* header, CDC 1.10 */
    5, 0x24, 0x01, 0x00, 1,             /* call management: data on if 1 */
    4, 0x24, 0x02, 0x02,                /* ACM: line coding, serial state */
    5, 0x24, 0x06, 0, 1,                /* union: 0 controls 1 */
    7, 5, 0x81, 0x03, 8, 0, 16,         /* EP1 IN, interrupt */
    /* interface 1: data class, bulk OUT and IN */
    9, 4, 1, 0, 2, 0x0a, 0x00, 0x00, 0,
    7, 5, 0x02, 0x02, BULK_SIZE, 0, 0,  /* EP2 OUT, bulk */
    7, 5, 0x82, 0x02, BULK_SIZE, 0, 0   /* EP2 IN, bulk */
};

static const char *const strings[] = {
    NULL,                   /* 0: language list, built separately */
    "pTOS",
    "pTOS console (RP2350)",
    "0001"
};

/* Output ring, filled from the very first character */
#define TX_RING_SIZE 4096
static UBYTE tx_ring[TX_RING_SIZE];
static volatile UWORD tx_head, tx_tail;

/* Input ring, only used when the console input is not fed to the IKBD */
#define RX_RING_SIZE 256
static UBYTE rx_ring[RX_RING_SIZE];
static volatile UWORD rx_head, rx_tail;

/* not initialized here: .data is read-only (it lives in flash) */
static UBYTE line_coding[7];        /* dwDTERate, bCharFormat, bParityType, bDataBits */
static UBYTE desc_buf[64];

static volatile BOOL usb_up;        /* rp2350_usbcon_init() has run */
static volatile BOOL configured;
static volatile BOOL dtr;           /* a terminal has the port open */
static volatile BOOL tx_busy;
static BOOL in_poll;

static UBYTE pending_addr;
static BOOL set_addr_pending;
static BOOL line_coding_pending;
static const UBYTE *ep0_tx_ptr;
static UWORD ep0_tx_left;
static BOOL ep0_tx_zlp;             /* a zero-length packet must end the data */
static ULONG ep0_pid;               /* next PID on EP0 IN */
static ULONG ep2_in_pid, ep2_out_pid;

/*
 * The controller runs from the 48 MHz USB clock.  When a buffer is handed
 * over, AVAILABLE must be set only after the other fields have settled on
 * its side (RP2350 datasheet, 12.7.3.2): write it in a second step.
 */
static void buf_ctrl_write(ULONG off, ULONG value)
{
    BUF_CTRL(off) = value & ~BUF_AVAIL;
    if (value & BUF_AVAIL)
    {
        __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\t"
                          "nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop");
        BUF_CTRL(off) = value;
    }
}

static void ep0_send_chunk(void)
{
    UWORD len = ep0_tx_left > EP0_SIZE ? EP0_SIZE : ep0_tx_left;
    volatile UBYTE *buf = DPRAM_PTR(EP0_BUF);
    UWORD i;

    for (i = 0; i < len; i++)
        buf[i] = ep0_tx_ptr[i];
    ep0_tx_ptr += len;
    ep0_tx_left -= len;
    if (len < EP0_SIZE)
        ep0_tx_zlp = FALSE;         /* a short packet ends the transfer */

    buf_ctrl_write(EP0_IN_BUF_CTRL, len | BUF_FULL | ep0_pid | BUF_AVAIL);
    ep0_pid ^= BUF_DATA1;
}

/* data stage device-to-host: len bytes of data, host asked for wlength */
static void ep0_send(const UBYTE *data, UWORD len, UWORD wlength)
{
    if (len > wlength)
        len = wlength;
    ep0_tx_ptr = data;
    ep0_tx_left = len;
    /* a transfer shorter than requested that is a multiple of the packet
     * size needs a zero-length packet to end it */
    ep0_tx_zlp = (len < wlength) && (len % EP0_SIZE == 0);
    ep0_send_chunk();
}

/* status stage (or empty data stage) device-to-host */
static void ep0_send_zlp(void)
{
    ep0_tx_ptr = NULL;
    ep0_tx_left = 0;
    ep0_tx_zlp = FALSE;
    buf_ctrl_write(EP0_IN_BUF_CTRL, BUF_FULL | BUF_DATA1 | BUF_AVAIL);
}

/* receive on EP0: the status stage after an IN data stage, or OUT data */
static void ep0_receive(void)
{
    buf_ctrl_write(EP0_OUT_BUF_CTRL, EP0_SIZE | BUF_DATA1 | BUF_AVAIL);
}

static void ep0_stall(void)
{
    USB_REG_SET(EP_STALL_ARM) = EP0_IN_BIT | EP0_OUT_BIT;
    BUF_CTRL(EP0_IN_BUF_CTRL) = BUF_STALL;
    BUF_CTRL(EP0_OUT_BUF_CTRL) = BUF_STALL;
}

static UWORD build_string_desc(int index)
{
    const char *s;
    UWORD len = 2;

    desc_buf[1] = DESC_STRING;
    if (index == 0)
    {
        desc_buf[2] = 0x09;         /* English (US) */
        desc_buf[3] = 0x04;
        len = 4;
    }
    else
    {
        for (s = strings[index]; *s && len < sizeof(desc_buf) - 1; s++)
        {
            desc_buf[len++] = (UBYTE)*s;
            desc_buf[len++] = 0;
        }
    }
    desc_buf[0] = (UBYTE)len;

    return len;
}

/*
 * Reboot into the bootrom's USB mass storage mode, through the bootrom's
 * own reboot() (RP2350 datasheet, 5.4 "Bootrom APIs"): the 16-bit pointer
 * at 0x16 is rom_table_lookup(code, mask).
 */
#define BAUD_BOOTSEL                1200
#define ROM_FUNC_REBOOT             ('R' | ('B' << 8))
#define RT_FLAG_FUNC_ARM_SEC        0x0004
#define REBOOT_TYPE_BOOTSEL         0x0002
#define REBOOT_NO_RETURN_ON_SUCCESS 0x0100

static void reboot_to_bootsel(void)
{
    typedef void *(*lookup_fn)(ULONG code, ULONG mask);
    typedef int (*reboot_fn)(ULONG flags, ULONG delay_ms, ULONG p0, ULONG p1);
    ULONG table_lookup = 0x16;
    lookup_fn lookup;
    reboot_fn reboot;

    /* the bootrom lives at address 0: keep GCC from treating this as a
     * NULL pointer dereference */
    __asm__ ("" : "+r"(table_lookup));
    lookup = (lookup_fn)(ULONG)*(volatile UWORD *)table_lookup;
    reboot = (reboot_fn)lookup(ROM_FUNC_REBOOT, RT_FLAG_FUNC_ARM_SEC);

    /* the delay lets the status stage of this very request complete */
    if (reboot)
        reboot(REBOOT_TYPE_BOOTSEL | REBOOT_NO_RETURN_ON_SUCCESS, 50, 0, 0);
}

static void rx_arm(void)
{
    buf_ctrl_write(EP2_OUT_BUF_CTRL, BULK_SIZE | ep2_out_pid | BUF_AVAIL);
    ep2_out_pid ^= BUF_DATA1;
}

/* start sending the next part of the output ring, if possible */
static void tx_kick(void)
{
    volatile UBYTE *buf = DPRAM_PTR(EP2_IN_BUF);
    UWORD len = 0;

    if (!configured || tx_busy || tx_head == tx_tail)
        return;

    while (len < BULK_SIZE && tx_tail != tx_head)
    {
        buf[len++] = tx_ring[tx_tail];
        tx_tail = (tx_tail + 1) % TX_RING_SIZE;
    }
    tx_busy = TRUE;
    buf_ctrl_write(EP2_IN_BUF_CTRL, len | BUF_FULL | ep2_in_pid | BUF_AVAIL);
    ep2_in_pid ^= BUF_DATA1;
}

static void set_configuration(UWORD value)
{
    configured = (value != 0);
    if (!configured)
        return;

    EP_CTRL(EP1_IN_CTRL) = EP_CTRL_ENABLE | EP_CTRL_INT_PER_BUFF
                         | EP_CTRL_TYPE_INTERRUPT | EP1_IN_BUF;
    EP_CTRL(EP2_IN_CTRL) = EP_CTRL_ENABLE | EP_CTRL_INT_PER_BUFF
                         | EP_CTRL_TYPE_BULK | EP2_IN_BUF;
    EP_CTRL(EP2_OUT_CTRL) = EP_CTRL_ENABLE | EP_CTRL_INT_PER_BUFF
                          | EP_CTRL_TYPE_BULK | EP2_OUT_BUF;
    ep2_in_pid = 0;
    ep2_out_pid = 0;
    tx_busy = FALSE;
    rx_arm();
}

static void handle_setup(void)
{
    volatile UBYTE *p = DPRAM_PTR(0);
    UBYTE type = p[0];
    UBYTE request = p[1];
    UWORD value = p[2] | (p[3] << 8);
    UWORD length = p[6] | (p[7] << 8);
    static UBYTE reply[2];

    ep0_pid = BUF_DATA1;            /* the data stage starts with DATA1 */
    line_coding_pending = FALSE;

    if ((type & 0x60) == 0x00)      /* standard request */
    {
        switch (request)
        {
        case REQ_GET_DESCRIPTOR:
            switch (value >> 8)
            {
            case DESC_DEVICE:
                ep0_send(device_desc, sizeof(device_desc), length);
                return;
            case DESC_CONFIG:
                ep0_send(config_desc, sizeof(config_desc), length);
                return;
            case DESC_STRING:
                if ((value & 0xff) < ARRAY_SIZE(strings))
                {
                    ep0_send(desc_buf, build_string_desc(value & 0xff), length);
                    return;
                }
                break;
            }
            break;
        case REQ_SET_ADDRESS:
            /* the address only takes effect after the status stage */
            pending_addr = value & 0x7f;
            set_addr_pending = TRUE;
            ep0_send_zlp();
            return;
        case REQ_SET_CONFIGURATION:
            set_configuration(value);
            ep0_send_zlp();
            return;
        case REQ_GET_CONFIGURATION:
            reply[0] = configured ? 1 : 0;
            ep0_send(reply, 1, length);
            return;
        case REQ_GET_STATUS:
            reply[0] = reply[1] = 0;
            ep0_send(reply, 2, length);
            return;
        default:
            if (!(type & 0x80))     /* acknowledge other host-to-device ones */
            {
                ep0_send_zlp();
                return;
            }
            break;
        }
    }
    else if ((type & 0x60) == 0x20) /* class request */
    {
        switch (request)
        {
        case CDC_SET_LINE_CODING:
            line_coding_pending = TRUE;
            ep0_receive();
            return;
        case CDC_GET_LINE_CODING:
            ep0_send(line_coding, sizeof(line_coding), length);
            return;
        case CDC_SET_CONTROL_LINE_STATE:
            dtr = (value & 1) != 0;
            ep0_send_zlp();
            if (dtr)
                tx_kick();
            return;
        case CDC_SEND_BREAK:
            ep0_send_zlp();
            return;
        }
    }

    ep0_stall();
}

static void rx_bytes(void)
{
    ULONG ctrl = BUF_CTRL(EP2_OUT_BUF_CTRL);
    UWORD len = ctrl & BUF_LEN_MASK;
    volatile UBYTE *buf = DPRAM_PTR(EP2_OUT_BUF);
    UWORD i;

    for (i = 0; i < len; i++)
    {
#if CONF_SERIAL_CONSOLE
        push_ascii_ikbdiorec(buf[i]);
#else
        UWORD next = (rx_head + 1) % RX_RING_SIZE;
        if (next != rx_tail)
        {
            rx_ring[rx_head] = buf[i];
            rx_head = next;
        }
#endif
    }
    rx_arm();
}

static void usb_service(void)
{
    ULONG ints = USB_REG(INTS);

    if (ints & INT_SETUP_REQ)
    {
        USB_REG(SIE_STATUS) = SIE_STATUS_SETUP_REC;
        handle_setup();
    }

    if (ints & INT_BUFF_STATUS)
    {
        ULONG status = USB_REG(BUFF_STATUS);

        if (status & EP0_IN_BIT)
        {
            USB_REG(BUFF_STATUS) = EP0_IN_BIT;
            if (set_addr_pending)
            {
                USB_REG(ADDR_ENDP) = pending_addr;
                set_addr_pending = FALSE;
            }
            else if (ep0_tx_left || ep0_tx_zlp)
                ep0_send_chunk();
            else if (ep0_tx_ptr)
            {
                /* data stage done: the host sends the status stage */
                ep0_tx_ptr = NULL;
                ep0_receive();
            }
        }
        if (status & EP0_OUT_BIT)
        {
            USB_REG(BUFF_STATUS) = EP0_OUT_BIT;
            if (line_coding_pending)
            {
                volatile UBYTE *buf = DPRAM_PTR(EP0_BUF);
                int i;

                for (i = 0; i < (int)sizeof(line_coding); i++)
                    line_coding[i] = buf[i];
                line_coding_pending = FALSE;
                ep0_send_zlp();
                if ((line_coding[0] | (line_coding[1] << 8) | ((ULONG)line_coding[2] << 16)
                     | ((ULONG)line_coding[3] << 24)) == BAUD_BOOTSEL)
                    reboot_to_bootsel();
            }
        }
        if (status & EP2_IN_BIT)
        {
            USB_REG(BUFF_STATUS) = EP2_IN_BIT;
            tx_busy = FALSE;
            tx_kick();
        }
        if (status & EP2_OUT_BIT)
        {
            USB_REG(BUFF_STATUS) = EP2_OUT_BIT;
            rx_bytes();
        }
        /* anything else (EP1 IN is never used) */
        status = USB_REG(BUFF_STATUS);
        if (status)
            USB_REG(BUFF_STATUS) = status;
    }

    if (ints & INT_BUS_RESET)
    {
        USB_REG(SIE_STATUS) = SIE_STATUS_BUS_RESET;
        USB_REG(ADDR_ENDP) = 0;
        configured = FALSE;
        dtr = FALSE;
        tx_busy = FALSE;
        set_addr_pending = FALSE;
        ep0_tx_ptr = NULL;
    }
}

static void usb_irq(void)
{
    in_poll = TRUE;
    usb_service();
    in_poll = FALSE;
}

/* Drive the controller by hand, when interrupts cannot do it. */
static void usb_poll(void)
{
    ULONG primask;

    if (!usb_up || in_poll)
        return;
    __asm__ volatile ("mrs %0, primask\n\tcpsid i" : "=r"(primask) : : "memory");
    usb_irq();
    __asm__ volatile ("msr primask, %0" : : "r"(primask) : "memory");
}

void rp2350_usbcon_putc(UBYTE c)
{
    UWORD next = (tx_head + 1) % TX_RING_SIZE;

    while (next == tx_tail)
    {
        /* Ring full.  Nobody listening: drop the character.  Otherwise
         * wait for the host to take data, servicing the controller
         * ourselves in case interrupts are masked. */
        if (!dtr || !configured || in_poll)
            return;
        usb_poll();
    }
    tx_ring[tx_head] = c;
    tx_head = next;

    if (usb_up)
    {
        ULONG primask;

        __asm__ volatile ("mrs %0, primask\n\tcpsid i" : "=r"(primask) : : "memory");
        tx_kick();
        __asm__ volatile ("msr primask, %0" : : "r"(primask) : "memory");
    }
}

BOOL rp2350_usbcon_can_read(void)
{
    return rx_head != rx_tail;
}

UBYTE rp2350_usbcon_getc(void)
{
    UBYTE c;

    while (rx_head == rx_tail)
        usb_poll();
    c = rx_ring[rx_tail];
    rx_tail = (rx_tail + 1) % RX_RING_SIZE;

    return c;
}

/*
 * Called once the interrupt controller is set up (rp2350_int_init()).
 * The controller clock (clk_usb, 48 MHz) and its reset are handled by
 * rp2350_board_init().
 */
void rp2350_usbcon_init(void)
{
    volatile ULONG *dpram = (volatile ULONG *)DPRAM;
    int i;

    for (i = 0; i < 4096 / 4; i++)
        dpram[i] = 0;

    line_coding[0] = 0x00;          /* 115200 baud, 8N1 */
    line_coding[1] = 0xc2;
    line_coding[2] = 0x01;
    line_coding[6] = 8;

    USB_REG(USB_MUXING) = USB_MUXING_TO_PHY | USB_MUXING_SOFTCON;
    /* no VBUS sense pin: the board is powered through this very port */
    USB_REG(USB_PWR) = USB_PWR_VBUS_DETECT | USB_PWR_VBUS_DETECT_OVERRIDE_EN;
    USB_REG(MAIN_CTRL) = MAIN_CTRL_CONTROLLER_EN;   /* also clears PHY_ISO */
    USB_REG(SIE_CTRL) = SIE_CTRL_EP0_INT_1BUF;
    USB_REG(INTE) = INT_SETUP_REQ | INT_BUS_RESET | INT_BUFF_STATUS;

    usb_up = TRUE;
    rp2350_connect_irq(RP2350_USBCTRL_IRQ, usb_irq);

    /* present ourselves to the host */
    USB_REG_SET(SIE_CTRL) = SIE_CTRL_PULLUP_EN;
}
