/*
 * wifisettings.c - the network the machine is told to join
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This software is licenced under the GNU Public License.
 * Please see LICENSE.TXT for further information.
 *
 * Two strings, and a cookie so that programs can find them: the name of
 * a wireless network and its key.  The desktop asks for them ("Wifi
 * settings..." under Options) and saves them with the rest of its
 * settings; a program that wants to talk to a radio module asks here
 * rather than asking the user again.
 *
 * The strings live in the BIOS because the desktop comes and goes -- it
 * ends whenever a program runs -- while the cookie jar stays.
 *
 * See include/wifi.h for what a program sees, including what to think of
 * a key kept in plain text.
 */

#include "emutos.h"
#include "cookie.h"
#include "string.h"
#include "wifi.h"
#include "wifisettings.h"

#if CONF_WITH_WIFI_SETTINGS

static char wifi_ssid_buf[WIF_SSID_LEN + 1];
static char wifi_key_buf[WIF_KEY_LEN + 1];
static short wifi_utc_offset = 1;       /* Central European Time */

const char *wifi_get_ssid(void)
{
    return wifi_ssid_buf;
}

const char *wifi_get_key(void)
{
    return wifi_key_buf;
}

BOOL wifi_configured(void)
{
    return wifi_ssid_buf[0] != '\0';
}

void wifi_set(const char *ssid, const char *key)
{
    strlcpy(wifi_ssid_buf, ssid ? ssid : "", sizeof(wifi_ssid_buf));
    strlcpy(wifi_key_buf, key ? key : "", sizeof(wifi_key_buf));
}

short wifi_get_utc_offset(void)
{
    return wifi_utc_offset;
}

void wifi_set_utc_offset(short hours)
{
    if (hours >= -12 && hours <= 14)    /* the world's real range */
        wifi_utc_offset = hours;
}

/* "1", "+2", "-5": what the dialog and EMUDESK.INF carry */
void wifi_set_utc_offset_str(const char *s)
{
    short sign = 1, value = 0;

    if (!s || !*s)
        return;                         /* nothing said: keep what we have */
    while (*s == ' ')
        s++;
    if (!*s)
        return;
    if (*s == '-')
    {
        sign = -1;
        s++;
    }
    else if (*s == '+')
        s++;
    while (*s >= '0' && *s <= '9')
        value = (short)(value * 10 + (*s++ - '0'));

    wifi_set_utc_offset((short)(sign * value));
}

/* ---- what a program sees ---- */

static short wif_set(const char *ssid, const char *key)
{
    if (!ssid)
        return -1;
    if (strlen(ssid) > WIF_SSID_LEN || (key && strlen(key) > WIF_KEY_LEN))
        return -1;
    wifi_set(ssid, key);
    return 0;
}

static short wif_set_utc(short hours)
{
    wifi_set_utc_offset(hours);
    return 0;
}

static const struct wif_api wif_api = {
    WIF_VERSION,
    sizeof(struct wif_api),
    wifi_get_ssid,
    wifi_get_key,
    wif_set,
    wifi_get_utc_offset,
    wif_set_utc
};

void wifi_add_cookie(void)
{
    cookie_add(WIF_COOKIE, (ULONG)&wif_api);
}

#endif /* CONF_WITH_WIFI_SETTINGS */
