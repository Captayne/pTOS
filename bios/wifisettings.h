/*
 * wifisettings.h - the network the machine is told to join, inside pTOS
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This software is licenced under the GNU Public License.
 * Please see LICENSE.TXT for further information.
 */

#ifndef WIFISETTINGS_H
#define WIFISETTINGS_H

const char *wifi_get_ssid(void);
const char *wifi_get_key(void);
BOOL wifi_configured(void);
void wifi_set(const char *ssid, const char *key);
void wifi_add_cookie(void);

#endif /* WIFISETTINGS_H */
