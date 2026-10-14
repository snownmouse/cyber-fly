// SPDX-License-Identifier: Apache-2.0
//
// Board-side Wi-Fi station for the cyber fly's brain link.
//
// Ported from tests/firmware/hw_probe/main/wifi_sta.c (Task A). Three things in
// there were hard-won and are carried over deliberately:
//
//  1. SYMBOL PREFIX. A bare `wifi_sta_disconnect` collides with a same-named
//     symbol inside the prebuilt libnet80211.a (ieee80211_ioctl.o) and fails the
//     ELF link with "multiple definition". Found by diffing `nm --defined-only -g`
//     output of our libmain.a against every .a in the IDF tree. Hence flywifi_*.
//  2. INVALID_STATE IS NOT AN ERROR. Once an association times out the driver
//     stays initialised, so every later esp_wifi_init() returns
//     ESP_ERR_INVALID_STATE. Treating that as fatal means "try once, never
//     again"; instead bring the radio down and drive it from this call.
//  3. UNBOUNDED RETRIES. The original probe gave up after N attempts. Here the
//     link task keeps trying forever, because the board may well boot before the
//     phone hotspot is up, and a demo that never recovers on its own is worse
//     than one that logs a warning every 30 s.
//
// The disconnect reason code is logged because it is the only cheap way to tell
// "hotspot off" from "wrong password" - 15 is a handshake timeout, 201 is
// NO_AP_FOUND (which on a 2.4 GHz-only radio also means "the AP is 5 GHz only").
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool    connected;
    char    ip[16];
    int8_t  rssi;
    uint8_t channel;
    uint32_t connect_ms;
} fly_wifi_info_t;

/**
 * @brief Join @p ssid and wait for a DHCP lease, retrying forever.
 *
 * Returns ESP_OK only once an address is bound. Returns ESP_ERR_INVALID_ARG for
 * an empty SSID (a compile-time misconfiguration, not a link problem).
 *
 * @p retry_sec is the pause between attempts. Pass 0 for the built-in default.
 * Keep `password` correct: an empty password against a WPA2 AP retries forever
 * and looks exactly like a dead hotspot.
 */
esp_err_t flywifi_connect(const char *ssid, const char *password,
                          int timeout_sec, int retry_sec, fly_wifi_info_t *out);

/** True once a lease has been obtained and not since lost. */
bool flywifi_is_connected(void);

/** Stop the station and release the driver. Safe when not connected. */
void flywifi_disconnect(void);

#ifdef __cplusplus
}
#endif
