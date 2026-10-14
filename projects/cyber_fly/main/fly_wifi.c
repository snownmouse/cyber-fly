// SPDX-License-Identifier: Apache-2.0
//
// See fly_wifi.h for the three Task A findings this port preserves.
#include "fly_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "fly_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1

/* The link task retries forever, so the only thing that ends a wait is success
 * or the per-attempt association timeout. */
#define WIFI_MAX_ATTEMPTS  0
#define WIFI_HEARTBEAT_MS  30000u

static EventGroupHandle_t s_event_group;
static esp_netif_t       *s_netif;
static int                s_retry_count;
static bool               s_handlers_registered;
static volatile bool      s_connected;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)data;
        const int reason = disc ? disc->reason : -1;
        s_connected = false;
        /* Keep trying. Rate-limit the log so a hotspot that is off for an hour
         * does not fill the log ring and evict everything else (Task A hit
         * exactly that: the Wi-Fi driver alone sprayed ~40 lines per attempt). */
        if (s_retry_count == 0 || s_retry_count % 10 == 0) {
            ESP_LOGW(TAG, "断连 reason=%d（第 %d 次），继续重试", reason, s_retry_count + 1);
        }
        s_retry_count++;
        esp_wifi_connect();
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "已获取 IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_connected = true;
        if (s_event_group) {
            xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

/*
 * ESP-Iris may already have created the netif layer and the default event loop
 * before on_start() runs, so "already initialised" must be treated as success.
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

/* One association attempt. Returns ESP_OK when a lease arrives. */
static esp_err_t wifi_attempt(const char *ssid, const char *password, int timeout_sec)
{
    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "create default Wi-Fi STA netif failed");
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t init_err = esp_wifi_init(&init_cfg);
    if (init_err == ESP_ERR_INVALID_STATE) {
        /*
         * A previous attempt left the driver initialised, and esp_wifi_init()
         * refuses to run twice. esp_wifi_stop() is NOT enough here: it leaves
         * the driver in the INITIALIZED state, so the next esp_wifi_init()
         * returns INVALID_STATE again and the retry loop spins forever without
         * ever associating. esp_wifi_deinit() is the only call that returns the
         * driver to UNINITIALIZED, which is what makes a retry possible at all.
         *
         * (Task A's wifi_sta.c stopped without deinit-ing, which was harmless
         * there because the probe rebooted after one attempt. An infinitely
         * retrying caller has to deinit.)
         */
        ESP_LOGW(TAG, "Wi-Fi 驱动已初始化，先 deinit 再重试");
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_deinit();
        init_err = esp_wifi_init(&init_cfg);
    }
    if (init_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(init_err));
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
    /*
     * WPA2-PSK is the realistic case, so do NOT relax the auth mode the way the
     * hw_probe bench code did. WIFI_AUTH_OPEN means "accept whatever the AP
     * offers", which on a password-protected hotspot means the driver will
     * happily attempt an open association and never send the PSK - the AP drops
     * it, and the board retries forever. Accept anything instead.
     */
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG,
                        "esp_wifi_set_config failed");

    /*
     * Drop any lease left over from a previous attempt. Without this, a retry
     * after a stale-but-present address can report success while the traffic
     * still has nowhere to go, and the failure looks like "connected but
     * nothing works" instead of "never connected".
     */
    if (s_connected) {
        esp_wifi_disconnect();
    }
    esp_netif_ip_info_t old_ip = { 0 };
    if (s_netif && esp_netif_get_ip_info(s_netif, &old_ip) == ESP_OK && old_ip.ip.addr != 0) {
        ESP_LOGW(TAG, "清除旧租约 " IPSTR, IP2STR(&old_ip.ip));
        esp_netif_destroy(s_netif);          // returns void: do not assign it
        s_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "recreate netif failed");
    }
    s_connected = false;

    xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);

    const int64_t started_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    /* Modem power save naps between beacons and adds tens of milliseconds of
     * apparent latency that has nothing to do with the link. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable Wi-Fi power save failed");

    const EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                                 WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                                 pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS((timeout_sec > 0 ? timeout_sec : 20) * 1000));
    (void)started_us;
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t flywifi_connect(const char *ssid, const char *password, int timeout_sec,
                          int retry_sec, fly_wifi_info_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGE(TAG, "Wi-Fi SSID 为空（Kconfig 里没填？）");
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * An empty password against a protected AP fails in a way that looks
     * identical to "the hotspot is off": the driver associates, never gets a
     * lease, and retries silently forever. Say so once, loudly, instead of
     * making the operator guess from a repeating timeout.
     */
    if (!password || password[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi 密码为空——若热点加密，这会一直连不上（Kconfig: FLY_WIFI_PASSWORD）");
    } else {
        ESP_LOGI(TAG, "连接目标 SSID=%s 密码长度=%d", ssid, (int)strlen(password));
    }

    ESP_RETURN_ON_ERROR(init_network_stack(), TAG, "network stack init failed");

    if (!s_event_group) {
        s_event_group = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_event_group, ESP_ERR_NO_MEM, TAG, "event group alloc failed");
    }
    s_retry_count = 0;

    const int64_t t0 = esp_timer_get_time();
    for (;;) {
        if (wifi_attempt(ssid, password, timeout_sec) == ESP_OK) {
            if (out) {
                wifi_ap_record_t ap = { 0 };
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                    out->rssi = ap.rssi;
                    out->channel = ap.primary;
                }
                esp_netif_ip_info_t ip_info = { 0 };
                if (esp_netif_get_ip_info(s_netif, &ip_info) == ESP_OK) {
                    snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip_info.ip));
                }
                out->connected = true;
                out->connect_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
            }
            ESP_LOGI(TAG, "已连接 %s（%u ms，%u 次尝试）", ssid,
                     (unsigned)out->connect_ms, (unsigned)(s_retry_count + 1));
            return ESP_OK;
        }

        /* Back off so an absent hotspot does not spin the radio at full rate. */
        const uint32_t wait_ms = (retry_sec > 0 ? (uint32_t)retry_sec : WIFI_HEARTBEAT_MS / 1000u)
                                 * 1000u;
        ESP_LOGW(TAG, "未连上，%u 秒后再试", (unsigned)(wait_ms / 1000u));
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

bool flywifi_is_connected(void)
{
    return s_connected;
}

void flywifi_disconnect(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
    /* deinit, not just stop: leaving the driver INITIALIZED is what makes a
     * later esp_wifi_init() fail with ESP_ERR_INVALID_STATE. */
    esp_wifi_deinit();
    s_connected = false;
}
