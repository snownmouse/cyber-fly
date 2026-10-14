/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wifi_sta.h
 * @brief Minimal Wi-Fi station connection used by the UDP link probe.
 *
 * ESP32-S31 has a 2.4 GHz-only radio, so the access point must publish a
 * 2.4 GHz SSID. Modem power save is turned off explicitly: with it on, the
 * reported jitter reflects the power-save schedule rather than the link.
 */

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
    char    netmask[16];
    char    gateway[16];
    int8_t  rssi;
    uint8_t channel;
    uint8_t bssid[6];
    uint32_t connect_ms;
} wifi_sta_result_t;

/*
 * Both entry points carry an hwprobe_ prefix on purpose: a bare
 * `wifi_sta_disconnect` collides with a same-named symbol inside the prebuilt
 * libnet80211.a (ieee80211_ioctl.o) and fails the ELF link with "multiple
 * definition". Checked with nm against every .a in the IDF tree -- these are
 * the only probe symbols that must stay distinct.
 */

/**
 * @brief Join an access point and wait for a DHCP lease.
 *
 * @param ssid          Target SSID (2.4 GHz).
 * @param password      Passphrase, or an empty string for an open network.
 * @param timeout_sec   How long to wait for an IP address.
 * @param max_retry     Reconnect attempts after a disconnect.
 * @param out           Receives connection details. May be NULL.
 */
esp_err_t hwprobe_wifi_connect(const char *ssid, const char *password, int timeout_sec,
                               int max_retry, wifi_sta_result_t *out);

/** Stop the station and release the driver. Safe to call when not connected. */
void hwprobe_wifi_disconnect(void);

/**
 * @brief Diagnostic: scan and log every 2.4 GHz AP currently audible.
 *
 * Leaves the driver running; a following hwprobe_wifi_connect() restarts it.
 * Returns ESP_OK even when nothing is found (that is itself the answer).
 */
esp_err_t hwprobe_wifi_scan(void);

#ifdef __cplusplus
}
#endif
