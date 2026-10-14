// SPDX-License-Identifier: Apache-2.0
//
// See fly_mic.h for what was ported from Task A's mic_probe.c and what was left
// behind, and why.
#include "fly_mic.h"

#include <math.h>
#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "fly_mic";

#define MIC_SAMPLE_RATE     16000
#define MIC_CHANNELS        1
#define MIC_BITS            16
/* One control frame is 50 ms; at 16 kHz mono that is exactly 800 samples, so the
 * uplink frame period and the audio window are the same number by construction -
 * no resampling, no queue. */
#define MIC_FEATURE_SAMPLES 800
#define MIC_FULL_SCALE      32768.0

/* Band edges inherited verbatim from mic_probe.c: they were chosen to line up
 * with the Johnston's organ split (JO-B low, JO-A mid, broadband above). */
static const float k_band_low_hz[]  = { 50.0f, 100.0f, 150.0f };
static const float k_band_mid_hz[]  = { 250.0f, 350.0f, 450.0f };
static const float k_band_high_hz[] = { 600.0f, 1000.0f, 1500.0f };

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Same conversion as Task A's probe_dbfs_from_amplitude(), inlined so this file
 * does not have to pull in probe_common.h just for one function. */
static float amplitude_to_dbfs(double amplitude, double full_scale)
{
    if (amplitude <= 0.0) {
        return -120.0f;
    }
    const double ratio = amplitude / full_scale;
    if (ratio <= 0.0) {
        return -120.0f;
    }
    return (float)(20.0 * log10(ratio));
}

static float goertzel_dbfs(const int16_t *samples, size_t count, float freq)
{
    const float omega = 2.0f * (float)M_PI * freq / (float)MIC_SAMPLE_RATE;
    const float cos_omega = cosf(omega);
    const float sin_omega = sinf(omega);
    const float coeff = 2.0f * cos_omega;
    float q1 = 0.0f;
    float q2 = 0.0f;

    for (size_t i = 0; i < count; i++) {
        const float q0 = coeff * q1 - q2 + (float)samples[i];
        q2 = q1;
        q1 = q0;
    }

    const float real = q1 - q2 * cos_omega;
    const float imag = q2 * sin_omega;
    const float magnitude = sqrtf(real * real + imag * imag);
    /* The raw Goertzel result scales with N/2; normalise back to amplitude. */
    const double amplitude = (double)magnitude / ((double)count * 0.5);

    return amplitude_to_dbfs(amplitude, MIC_FULL_SCALE);
}

static float band_peak_dbfs(const float *freqs, size_t freq_count,
                            const int16_t *samples, size_t count)
{
    float peak = -120.0f;

    for (size_t i = 0; i < freq_count; i++) {
        const float level = goertzel_dbfs(samples, count, freqs[i]);
        if (level > peak) {
            peak = level;
        }
    }
    return peak;
}

/*
 * The Task A trap, preserved verbatim in intent: band_peak_dbfs() already
 * returns dBFS, so 10^(dBFS/20) is a fraction of full scale. Dividing by
 * MIC_FULL_SCALE again here would apply the normalisation twice and sink every
 * band by 20*log10(32768) = 90.3 dB. Hence full scale 1.0 here.
 */
static float band_level_dbfs(double sum_sq, uint32_t blocks)
{
    if (blocks == 0 || sum_sq <= 0.0) {
        return -120.0f;
    }
    return amplitude_to_dbfs(sqrt(sum_sq / (double)blocks), 1.0);
}

esp_err_t fly_mic_open(esp_codec_dev_handle_t *out_mic, int gain_db)
{
    ESP_RETURN_ON_FALSE(out_mic, ESP_ERR_INVALID_ARG, TAG, "out_mic is null");

    /* Provisions codec power rails, I2C and the whole ES8311 register set. */
    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    ESP_RETURN_ON_FALSE(mic, ESP_FAIL, TAG, "ES8311 microphone init failed");

    /* esp_codec_dev_open() takes a non-const pointer, so this cannot be const. */
    esp_codec_dev_sample_info_t info = {
        .sample_rate = MIC_SAMPLE_RATE,
        .bits_per_sample = MIC_BITS,
        .channel = MIC_CHANNELS,
        .channel_mask = 0,
    };
    /* The BSP brings I2S up as 48 kHz stereo; opening at 16 kHz mono makes
     * esp_codec_dev reconfigure the ES8311 clock dividers for us. */
    const int open_ret = esp_codec_dev_open(mic, &info);
    ESP_RETURN_ON_FALSE(open_ret == ESP_CODEC_DEV_OK, ESP_FAIL, TAG,
                        "open microphone failed (%d)", open_ret);

    const int gain_ret = esp_codec_dev_set_in_gain(mic, (float)gain_db);
    ESP_RETURN_ON_FALSE(gain_ret == ESP_CODEC_DEV_OK, ESP_FAIL, TAG,
                        "set input gain %d dB failed (%d)", gain_db, gain_ret);

    ESP_LOGI(TAG, "麦克风已打开：%d Hz / %d bit / %d 声道，增益 %d dB",
             MIC_SAMPLE_RATE, MIC_BITS, MIC_CHANNELS, gain_db);
    *out_mic = mic;
    return ESP_OK;
}

esp_err_t fly_mic_sample(esp_codec_dev_handle_t mic, fly_mic_features_t *out)
{
    ESP_RETURN_ON_FALSE(mic && out, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    const size_t block_bytes = MIC_FEATURE_SAMPLES * sizeof(int16_t);
    int16_t *block = heap_caps_malloc(block_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(block, ESP_ERR_NO_MEM, TAG, "audio buffer alloc failed");

    const int read_ret = esp_codec_dev_read(mic, block, (int)block_bytes);
    if (read_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "麦克风读取失败 (%d)", read_ret);
        heap_caps_free(block);
        memset(out, 0, sizeof(*out));
        out->valid = false;
        return ESP_FAIL;
    }

    /* Broadband: E[x^2] - dc^2 removes the bias in one pass, no second read. */
    double sum = 0.0;
    double sum_sq = 0.0;
    int16_t max_sample = INT16_MIN;
    int16_t min_sample = INT16_MAX;
    for (size_t i = 0; i < MIC_FEATURE_SAMPLES; i++) {
        const double s = (double)block[i];
        sum += s;
        sum_sq += s * s;
        if (block[i] > max_sample) max_sample = block[i];
        if (block[i] < min_sample) min_sample = block[i];
    }
    const double dc = sum / (double)MIC_FEATURE_SAMPLES;
    double ac_variance = (sum_sq / (double)MIC_FEATURE_SAMPLES) - (dc * dc);
    if (ac_variance < 0.0) {
        ac_variance = 0.0;
    }
    const double ac_rms = sqrt(ac_variance);
    const double peak_up = (double)max_sample - dc;
    const double peak_dn = dc - (double)min_sample;

    /* Band energies accumulated in the linear domain so a quiet band is not
     * swamped by the loud one. */
    const double low = pow(10.0, (double)band_peak_dbfs(k_band_low_hz,
                                                        ARRAY_SIZE(k_band_low_hz),
                                                        block, MIC_FEATURE_SAMPLES) / 20.0);
    const double mid = pow(10.0, (double)band_peak_dbfs(k_band_mid_hz,
                                                        ARRAY_SIZE(k_band_mid_hz),
                                                        block, MIC_FEATURE_SAMPLES) / 20.0);
    const double high = pow(10.0, (double)band_peak_dbfs(k_band_high_hz,
                                                         ARRAY_SIZE(k_band_high_hz),
                                                         block, MIC_FEATURE_SAMPLES) / 20.0);

    out->valid = true;
    out->rms_dbfs = amplitude_to_dbfs(ac_rms, MIC_FULL_SCALE);
    out->peak_dbfs = amplitude_to_dbfs(peak_up > peak_dn ? peak_up : peak_dn, MIC_FULL_SCALE);
    out->band_low_db = band_level_dbfs(low * low, 1);
    out->band_mid_db = band_level_dbfs(mid * mid, 1);
    out->band_high_db = band_level_dbfs(high * high, 1);

    heap_caps_free(block);
    return ESP_OK;
}
