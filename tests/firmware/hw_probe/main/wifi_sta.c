/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wifi_sta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "hw_probe";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAILED_BIT     BIT1

static EventGroupHandle_t s_event_group;
static esp_netif_t       *s_netif;
static int                s_retry_count;
static int                s_retry_limit;
static bool               s_started;
static bool               s_handlers_registered;
static volatile bool      s_scan_mode;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    /* While a scan owns the radio, do not let the driver's STA_START /
     * DISCONNECTED events kick off an association: esp_wifi_scan_start() is
     * refused with ESP_ERR_WIFI_STATE while the STA is connecting. */
    if (s_scan_mode) {
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* The reason code is the cheapest way to tell "hotspot off / out of
         * range" (201 NO_AP_FOUND) from "wrong password" (15 4WAY_HANDSHAKE_
         * TIMEOUT) from "5 GHz-only SSID" (also 201: a 2.4 GHz-only radio
         * cannot see it at all). */
        const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)data;
        const int reason = disc ? disc->reason : -1;
        if (s_retry_count < s_retry_limit) {
            s_retry_count++;
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d/%d", reason, s_retry_count,
                     s_retry_limit);
            esp_wifi_connect();
        } else if (s_event_group) {
            ESP_LOGE(TAG, "Wi-Fi gave up after %d attempts; last reason %d "
                          "(201 = SSID not seen: hotspot off, out of range, or 5 GHz only)",
                     s_retry_count, reason);
            xEventGroupSetBits(s_event_group, WIFI_FAILED_BIT);
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        if (s_event_group) {
            xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

/*
 * ESP-Iris may already have created the netif layer and the default event loop
 * before app_main() runs. Treat "already initialized" as success rather than
 * asserting, so the probe works whether or not that happened.
 */
static esp_err_t init_network_stack(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return ESP_OK;
}

esp_err_t hwprobe_wifi_connect(const char *ssid, const char *password, int timeout_sec,
                           int max_retry, wifi_sta_result_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGE(TAG, "Wi-Fi SSID is empty; set it in menuconfig under HW Probe");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(init_network_stack(), TAG, "network stack init failed");

    if (!s_event_group) {
        s_event_group = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_event_group, ESP_ERR_NO_MEM, TAG, "event group alloc failed");
    }

    s_retry_limit = max_retry > 0 ? max_retry : 1;
    s_retry_count = 0;

    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_sta();
    }
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "create default Wi-Fi STA netif failed");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    const esp_err_t init_err = esp_wifi_init(&init_cfg);
    if (init_err == ESP_ERR_INVALID_STATE) {
        /*
         * A previous attempt already initialised the driver -- typically because
         * an association timed out and left the radio up. Re-initialising is what
         * the driver refuses, not retrying, so bring it down and drive it again
         * from this call rather than giving up.
         */
        ESP_LOGW(TAG, "Wi-Fi driver already up; restarting it for this attempt");
        esp_wifi_disconnect();
        esp_wifi_stop();
    } else if (init_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(init_err));
        return init_err;
    }

    if (!s_handlers_registered) {
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                                wifi_event_handler, NULL, NULL),
                            TAG, "register Wi-Fi event handler failed");
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                                wifi_event_handler, NULL, NULL),
                            TAG, "register IP event handler failed");
        s_handlers_registered = true;
    }

    wifi_config_t wifi_config = { 0 };
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", ssid);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s",
             password ? password : "");
    /* Accept whatever the AP offers; this is a bench probe, not a product. */
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "esp_wifi_set_config failed");

    xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);

    const int64_t started_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
    s_started = true;

    /*
     * Power save off is required for meaningful jitter numbers. With the
     * default WIFI_PS_MIN_MODEM the radio naps between beacons and adds tens of
     * milliseconds of apparent latency that has nothing to do with the link.
     */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable Wi-Fi power save failed");

    const EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                                 WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                                 pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS((timeout_sec > 0 ? timeout_sec : 20) * 1000));

    if (bits & WIFI_CONNECTED_BIT) {
        wifi_ap_record_t ap = { 0 };
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            if (out) {
                out->rssi = ap.rssi;
                out->channel = ap.primary;
                memcpy(out->bssid, ap.bssid, sizeof(out->bssid));
            }
        }

        esp_netif_ip_info_t ip_info = { 0 };
        if (esp_netif_get_ip_info(s_netif, &ip_info) == ESP_OK) {
            if (out) {
                snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip_info.ip));
                snprintf(out->netmask, sizeof(out->netmask), IPSTR, IP2STR(&ip_info.netmask));
                snprintf(out->gateway, sizeof(out->gateway), IPSTR, IP2STR(&ip_info.gw));
            }
        }
        if (out) {
            out->connected = true;
            out->connect_ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);
        }
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Wi-Fi did not connect within %d s", timeout_sec > 0 ? timeout_sec : 20);
    return ESP_ERR_TIMEOUT;
}

void hwprobe_wifi_disconnect(void)
{
    if (!s_started) {
        return;
    }
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    s_started = false;
}

/* Radio-owning scan body. The caller must have set s_scan_mode first. */
static esp_err_t wifi_scan_locked(void)
{
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    const esp_err_t init_err = esp_wifi_init(&init_cfg);
    if (init_err == ESP_ERR_INVALID_STATE) {
        esp_wifi_disconnect();
        esp_wifi_stop();
    } else if (init_err != ESP_OK) {
        ESP_LOGE(TAG, "scan: esp_wifi_init failed: %s", esp_err_to_name(init_err));
        return init_err;
    }

    /* A scan is refused while the STA is associating ("STA is connecting, scan
     * are not allowed"), so bring the driver up with the connect handler
     * suppressed (s_scan_mode) and only then scan. */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan: radio start failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_scan_config_t scan_cfg = { 0 };
    err = esp_wifi_scan_start(&scan_cfg, true /* block until done */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan: esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK) {
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "scan: %u AP(s) visible on 2.4 GHz", (unsigned)ap_count);
    if (ap_count == 0) {
        ESP_LOGW(TAG, "scan: nothing heard at all -- radio up but no beacon in range");
        return ESP_OK;
    }

    if (ap_count > 32) {
        ap_count = 32;
    }
    wifi_ap_record_t *recs = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!recs) {
        return ESP_ERR_NO_MEM;
    }
    uint16_t got = ap_count;
    if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
        for (uint16_t i = 0; i < got; i++) {
            ESP_LOGW(TAG, "  AP[%u] \"%s\"  rssi=%d dBm  ch=%u  auth=%d",
                     (unsigned)i, (const char *)recs[i].ssid, (int)recs[i].rssi,
                     (unsigned)recs[i].primary, (int)recs[i].authmode);
        }
    }
    free(recs);
    return ESP_OK;
}

esp_err_t hwprobe_wifi_scan(void)
{
    /*
     * Passive diagnostic: list every AP the 2.4 GHz radio can actually hear.
     * "reason 201 (NO_AP_FOUND)" only says the configured SSID was not seen --
     * it does not say what *was* heard, which is what distinguishes "hotspot
     * off" from "renamed" from "5 GHz-only". The radio is left up; the
     * following hwprobe_wifi_connect() restarts the driver on its own.
     */
    ESP_RETURN_ON_ERROR(init_network_stack(), TAG, "scan: network stack init failed");

    s_scan_mode = true;
    const esp_err_t err = wifi_scan_locked();
    s_scan_mode = false;
    return err;
}
