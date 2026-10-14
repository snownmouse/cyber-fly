// SPDX-License-Identifier: Apache-2.0
#define GSP_BUNDLE_ENABLE_RAW_IDS 1
#include "bundle_gsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mosaico_game_2d.h"
#include "fly_app.h"
#include "fly_game.h"
#include "fly_link.h"
#include "fly_screen_mirror.h"
#include "fly_view.h"

#if CONFIG_FLY_LINK_ENABLE
#include "fly_wifi.h"
#endif

#ifndef CONFIG_FLY_LINK_ENABLE
#define CONFIG_FLY_LINK_ENABLE 1
#endif
#ifndef CONFIG_FLY_LINK_HOST
#define CONFIG_FLY_LINK_HOST "192.168.1.100"
#endif
#ifndef CONFIG_FLY_LINK_PORT
#define CONFIG_FLY_LINK_PORT 33333
#endif

static const char *TAG = "cyber_fly";
static fly_game_t s_fly;
static fly_link_t s_link;
static bool s_input;
static bool s_have_downlink;   // once true we never fall back to the fake driver
static TaskHandle_t s_link_task;

static void link_task(void *arg);

static esp_err_t on_start(void)
{
    fly_game_reset(&s_fly, 0x46595A49u);  // "FYBI"
    fly_link_reset(&s_link, (uint32_t)(esp_timer_get_time() / 1000));
    s_have_downlink = false;
#if CONFIG_FLY_LINK_ENABLE
    if (!s_link_task) {
        // 6 KB: the link task calls flywifi_connect(), which builds Wi-Fi driver
        // structures on the stack, and fly_mic_sample(), which runs a Goertzel
        // pass over 800 samples. 4 KB (the transport task's own stack) is not
        // enough for the connect path.
        if (xTaskCreate(link_task, "fly_bringup", 6144, NULL, 4, &s_link_task) != pdPASS) {
            ESP_LOGE(TAG, "脑下行启动任务创建失败");
            s_link_task = NULL;
        }
    }
#endif
    return ESP_OK;
}

static void link_task(void *arg)
{
    (void)arg;
    // Wi-Fi association and the codec open both happen HERE, not in on_start().
    // on_start() runs before the app is declared healthy, and ESP-Iris only
    // waits ~45 s for that (CONFIG_ESP_IRIS_HEALTH_TIMEOUT_MS): blocking on a
    // hotspot inside on_start() made the installer report outcome_unknown even
    // though the app came up fine. Off the critical path it costs nothing.
#if CONFIG_FLY_LINK_ENABLE
    if (fly_link_udp_start(&s_link, CONFIG_FLY_LINK_HOST,
                           (uint16_t)CONFIG_FLY_LINK_PORT) == ESP_OK) {
        ESP_LOGI(TAG, "脑下行已连接 %s:%d", CONFIG_FLY_LINK_HOST, CONFIG_FLY_LINK_PORT);
    } else {
        // Not fatal: the fly falls back to the built-in generator and keeps
        // animating, so a missing cloud link never bricks the demo.
        ESP_LOGW(TAG, "脑下行未启动，改用内置演示驱动");
    }
#endif
    s_link_task = NULL;
    vTaskDelete(NULL);
}

static void on_event(const mosaico_device_event_t *event)
{
    // The device reports contacts as TOUCH events whenever touch_points >= 2;
    // the host simulator (and single-point configs) arrive as POINTER.
    if (event->type != MOSAICO_DEVICE_EVENT_POINTER &&
        event->type != MOSAICO_DEVICE_EVENT_TOUCH) return;
    s_input = true;
    fly_game_pointer(&s_fly, event->x, event->y, event->pressed, 0);
}

static bool idle(void)
{
    // The cyber fly is always driven by the (fake) brain downlink, so it must
    // keep updating/rendering every frame. Never report idle.
    s_input = false;
    return false;
}

static void on_update(void)
{
    // One downlink frame per rendered frame. fly_link_take() hands over the
    // newest frame the link task has accepted since the last tick; a 30 fps UI
    // against a 20 Hz brain simply repeats the latest state on the extra ticks,
    // which is exactly what we want (the brain is the clock, not the screen).
    uint8_t buf[FLY_LINK_MPRC_LEN];
    if (fly_link_take(&s_link, buf)) {
        if (fly_game_push_downlink(&s_fly, buf, FLY_LINK_MPRC_LEN)) {
            s_have_downlink = true;
            // Report the link we actually measure, not the one the server
            // assumed when it encoded the frame.
            s_fly.state.link_q = fly_link_quality(&s_link,
                                                  (uint32_t)(esp_timer_get_time() / 1000));
        }
    } else if (!s_have_downlink) {
        // No cloud frame ever arrived: stay alive on the built-in generator.
        fly_fake_drive(&s_fly);
    }
    fly_game_update(&s_fly);
}

static void on_render(void)
{
    fly_view_render(&s_fly);
}

static void on_stats(void)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    // Name the failing stage instead of just saying "built-in": "no Wi-Fi" and
    // "cloud not replying" need completely different fixes, and on_stats is the
    // only line that survives in a --snapshot after the boot log rolled over.
    const char *src;
#if CONFIG_FLY_LINK_ENABLE
    if (s_have_downlink) {
        src = "云端";
    } else if (s_link_task) {
        src = "启动中";          // still associating / opening the codec
    } else if (fly_link_udp_running()) {
        src = "已发无回";         // Wi-Fi up, socket open, cloud silent
    } else {
        src = flywifi_is_connected() ? "无下行任务" : "未联网";
    }
#else
    src = "内置(编译期关闭)";
#endif
    ESP_LOGI(TAG,
             "源=%s tx=%lu rx=%lu 坏帧=%lu 短包=%lu 丢序=%lu RTT=%lums 质量=%u tick=%lu",
             src,
             (unsigned long)s_link.tx_frames, (unsigned long)s_link.rx_frames,
             (unsigned long)s_link.rx_rejects, (unsigned long)s_link.rx_short,
             (unsigned long)s_link.seq_gaps, (unsigned long)s_link.rtt_ms,
             (unsigned)fly_link_quality(&s_link, now), (unsigned long)s_fly.tick);
}

static const mosaico_game_app_config_t s_config = {
    .tag = "cyber_fly",
    .window_title = "Mosaico Cyber Fly",
    .canvas_bind = GSP_CYBER_FLY_BIND_GAME_CANVAS,
    .touch_points = 2,
    .enable_imu = false,
    .imu_sample_ms = 0,
    .target_fps = 30,
    .gsp_bundle = gsp_bundle_config,
    .register_mirror = fly_screen_mirror_register,
    .before_display = NULL,
    .on_start = on_start,
    .after_healthy = NULL,
    .on_event = on_event,
    .idle = idle,
    .on_update = on_update,
    .on_render = on_render,
    .on_stats = on_stats,
};

const mosaico_game_app_config_t *fly_app_config(void)
{
    return &s_config;
}
