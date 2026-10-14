/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mic_probe.h"

#include <math.h>
#include <string.h>

#include "probe_common.h"

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "hw_probe";

/*
 * Capture format. 16 kHz mono matches the planned feature cadence and keeps the
 * uplink to features rather than raw PCM. The ESP32-S31 has a 2.4 GHz-only
 * radio, so nothing here depends on a wide channel.
 */
#define MIC_SAMPLE_RATE     16000
#define MIC_CHANNELS        1
#define MIC_BITS            16
#define MIC_BLOCK_SAMPLES   1024                    /* 64 ms at 16 kHz */
#define MIC_FEATURE_SAMPLES 800                     /* 50 ms at 16 kHz */
#define MIC_SETTLE_BLOCKS   4                       /* ~256 ms discarded */
#define MIC_FULL_SCALE      32768.0

/* Clipping threshold: a little inside full scale so slow ramps still count. */
#define MIC_CLIP_LEVEL      32760

/* Band edges chosen to line up with the planned Johnston's organ encoding:
 * vibration-sensitive JO-B for the low band, JO-A for the mid band, and a
 * broadband onset cue above that. */
static const float k_band_low_hz[]  = { 50.0f, 100.0f, 150.0f };
static const float k_band_mid_hz[]  = { 250.0f, 350.0f, 450.0f };
static const float k_band_high_hz[] = { 600.0f, 1000.0f, 1500.0f };

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* -------------------------------------------------------------------------
 * Goertzel single-bin magnitude -> dBFS
 * ---------------------------------------------------------------------- */

static float goertzel_dbfs(const int16_t *samples, size_t count,
                           float sample_rate, float freq)
{
    const float omega = 2.0f * (float)M_PI * freq / sample_rate;
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

    return probe_dbfs_from_amplitude(amplitude, MIC_FULL_SCALE);
}

static float band_peak_dbfs(const float *freqs, size_t freq_count,
                            const int16_t *samples, size_t count)
{
    float peak = -120.0f;

    for (size_t i = 0; i < freq_count; i++) {
        const float level = goertzel_dbfs(samples, count, (float)MIC_SAMPLE_RATE, freqs[i]);
        if (level > peak) {
            peak = level;
        }
    }
    return peak;
}

static float band_rms_dbfs(const double *sum_sq, uint32_t blocks)
{
    if (blocks == 0 || *sum_sq <= 0.0) {
        return -120.0f;
    }
    /* sum_sq holds one full-scale-normalised amplitude per captured block, so
     * mean power is sum_sq / blocks. Passing a full scale of 1.0 keeps the one
     * normalisation band_peak_dbfs() already applied: it returns dBFS, so
     * 10^(dBFS/20) is already a fraction of full scale. Dividing by
     * MIC_FULL_SCALE again here would apply it twice and sink every band by
     * 20*log10(32768) = 90 dB, which is exactly the artifact that showed up as
     * band levels tens of dB below the broadband RMS. */
    const double mean_power = *sum_sq / (double)blocks;
    return probe_dbfs_from_amplitude(sqrt(mean_power), 1.0);
}

/* -------------------------------------------------------------------------
 * Block accumulator
 * ---------------------------------------------------------------------- */

typedef struct {
    uint64_t samples;
    double   sum;            /* for the DC bias        */
    double   sum_sq;         /* for the raw RMS        */
    int16_t  max_sample;
    int16_t  min_sample;
    uint32_t clipped;
    double   band_low_sq;    /* linear amplitude^2 sums, per band */
    double   band_mid_sq;
    double   band_high_sq;
    uint32_t blocks;
} mic_accum_t;

static void accum_reset(mic_accum_t *acc)
{
    memset(acc, 0, sizeof(*acc));
    acc->max_sample = INT16_MIN;
    acc->min_sample = INT16_MAX;
}

static void accum_add_block(mic_accum_t *acc, const int16_t *block, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const int16_t sample = block[i];

        acc->sum += (double)sample;
        acc->sum_sq += (double)sample * (double)sample;
        if (sample > acc->max_sample) {
            acc->max_sample = sample;
        }
        if (sample < acc->min_sample) {
            acc->min_sample = sample;
        }
        if (sample >= MIC_CLIP_LEVEL || sample <= -MIC_CLIP_LEVEL) {
            acc->clipped++;
        }
    }
    acc->samples += count;

    /* Band energies are accumulated in the linear domain so quiet bands are not
     * swamped by the loudest one. */
    const double low = (double)powf(10.0f, band_peak_dbfs(k_band_low_hz, ARRAY_SIZE(k_band_low_hz),
                                                          block, count) / 20.0f);
    const double mid = (double)powf(10.0f, band_peak_dbfs(k_band_mid_hz, ARRAY_SIZE(k_band_mid_hz),
                                                          block, count) / 20.0f);
    const double high = (double)powf(10.0f, band_peak_dbfs(k_band_high_hz, ARRAY_SIZE(k_band_high_hz),
                                                           block, count) / 20.0f);

    acc->band_low_sq += low * low;
    acc->band_mid_sq += mid * mid;
    acc->band_high_sq += high * high;
    acc->blocks++;
}

static void accum_finish(const mic_accum_t *acc, int gain_db, mic_analysis_t *out)
{
    memset(out, 0, sizeof(*out));

    if (acc->samples == 0) {
        out->valid = false;
        return;
    }

    const double n = (double)acc->samples;
    const double dc = acc->sum / n;
    const double raw_rms = sqrt(acc->sum_sq / n);

    /* E[(x - dc)^2] = E[x^2] - dc^2, so no second pass over the audio is needed. */
    double ac_variance = (acc->sum_sq / n) - (dc * dc);
    if (ac_variance < 0.0) {
        ac_variance = 0.0;
    }
    const double ac_rms = sqrt(ac_variance);

    const double peak_up = (double)acc->max_sample - dc;
    const double peak_down = dc - (double)acc->min_sample;
    const double peak = peak_up > peak_down ? peak_up : peak_down;

    out->valid = true;
    out->samples = (uint32_t)acc->samples;
    out->gain_db = gain_db;
    out->dc_offset = (float)dc;
    out->raw_rms_dbfs = probe_dbfs_from_amplitude(raw_rms, MIC_FULL_SCALE);
    out->ac_rms_dbfs = probe_dbfs_from_amplitude(ac_rms, MIC_FULL_SCALE);
    out->peak_dbfs = probe_dbfs_from_amplitude(peak, MIC_FULL_SCALE);
    out->crest_db = out->peak_dbfs - out->ac_rms_dbfs;
    out->clipped_samples = acc->clipped;
    out->clip_ratio = (float)((double)acc->clipped / n);
    out->band_low_db = band_rms_dbfs(&acc->band_low_sq, acc->blocks);
    out->band_mid_db = band_rms_dbfs(&acc->band_mid_sq, acc->blocks);
    out->band_high_db = band_rms_dbfs(&acc->band_high_sq, acc->blocks);
}

/* -------------------------------------------------------------------------
 * Codec control
 * ---------------------------------------------------------------------- */

esp_err_t mic_probe_open(esp_codec_dev_handle_t *out_mic, int gain_db)
{
    ESP_RETURN_ON_FALSE(out_mic, ESP_ERR_INVALID_ARG, TAG, "out_mic is null");

    /* bsp_audio_codec_microphone_init() provisions the codec power rails, the
     * I2C control path and the full ES8311 register set. Nothing here writes
     * codec registers by hand. */
    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    ESP_RETURN_ON_FALSE(mic, ESP_FAIL, TAG, "ES8311 microphone init failed");

    /* esp_codec_dev_open() takes a non-const pointer, so this cannot be const. */
    esp_codec_dev_sample_info_t info = {
        .sample_rate = MIC_SAMPLE_RATE,
        .bits_per_sample = MIC_BITS,
        .channel = MIC_CHANNELS,
        .channel_mask = 0,
    };
    /* The BSP brings I2S up as 48 kHz stereo; opening the device at 16 kHz mono
     * makes esp_codec_dev reconfigure the ES8311 clock dividers for us. */
    const int open_ret = esp_codec_dev_open(mic, &info);
    ESP_RETURN_ON_FALSE(open_ret == ESP_CODEC_DEV_OK, ESP_FAIL, TAG,
                        "open microphone failed (%d)", open_ret);

    ESP_RETURN_ON_ERROR(mic_probe_set_gain(mic, gain_db), TAG, "set input gain failed");

    ESP_LOGI(TAG, "Microphone open: %d Hz / %d bit / %d channel, gain %d dB",
             MIC_SAMPLE_RATE, MIC_BITS, MIC_CHANNELS, gain_db);
    *out_mic = mic;
    return ESP_OK;
}

esp_err_t mic_probe_set_gain(esp_codec_dev_handle_t mic, int gain_db)
{
    ESP_RETURN_ON_FALSE(mic, ESP_ERR_INVALID_ARG, TAG, "microphone handle is null");

    const int ret = esp_codec_dev_set_in_gain(mic, (float)gain_db);
    ESP_RETURN_ON_FALSE(ret == ESP_CODEC_DEV_OK, ESP_FAIL, TAG,
                        "esp_codec_dev_set_in_gain(%d) failed (%d)", gain_db, ret);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Capture
 * ---------------------------------------------------------------------- */

static esp_err_t capture_window(esp_codec_dev_handle_t mic, uint64_t wanted_samples, int gain_db,
                                size_t block_samples, bool settle, mic_analysis_t *out)
{
    ESP_RETURN_ON_FALSE(mic && out, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(block_samples > 0, ESP_ERR_INVALID_ARG, TAG, "block size is zero");

    /* A negative gain means "leave the codec wherever it currently is". */
    if (gain_db >= 0) {
        ESP_RETURN_ON_ERROR(mic_probe_set_gain(mic, gain_db), TAG, "set gain failed");
    }

    const size_t block_bytes = block_samples * sizeof(int16_t);
    int16_t *block = heap_caps_malloc(block_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(block, ESP_ERR_NO_MEM, TAG, "audio buffer alloc failed");

    mic_accum_t acc;
    accum_reset(&acc);

    /* Drop the first blocks: codec AGC and DC settling would otherwise be
     * counted as noise floor. */
    if (settle) {
        for (int i = 0; i < MIC_SETTLE_BLOCKS; i++) {
            (void)esp_codec_dev_read(mic, block, (int)block_bytes);
        }
    }

    while (acc.samples < wanted_samples) {
        const int read_ret = esp_codec_dev_read(mic, block, (int)block_bytes);
        if (read_ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "microphone read failed (%d), stopping capture", read_ret);
            break;
        }
        accum_add_block(&acc, block, block_samples);
    }

    heap_caps_free(block);

    if (acc.samples == 0) {
        memset(out, 0, sizeof(*out));
        out->valid = false;
        return ESP_FAIL;
    }

    accum_finish(&acc, gain_db, out);
    return ESP_OK;
}

esp_err_t mic_probe_measure(esp_codec_dev_handle_t mic, int seconds, int gain_db,
                            mic_analysis_t *out)
{
    ESP_RETURN_ON_FALSE(seconds > 0, ESP_ERR_INVALID_ARG, TAG, "seconds must be positive");
    const uint64_t wanted = (uint64_t)MIC_SAMPLE_RATE * (uint64_t)seconds;
    return capture_window(mic, wanted, gain_db, MIC_BLOCK_SAMPLES, true, out);
}

esp_err_t mic_probe_sample_features(esp_codec_dev_handle_t mic, mic_analysis_t *out)
{
    /* Exactly one 50 ms window, keeping the gain mic_probe_run() already set. */
    return capture_window(mic, MIC_FEATURE_SAMPLES, -1, MIC_FEATURE_SAMPLES, false, out);
}

/* -------------------------------------------------------------------------
 * Gain scan
 * ---------------------------------------------------------------------- */

static const int k_gain_table[] = { 0, 10, 20, 30, 40, 50 };

/* A floor at or below this is quiet enough that the gain can be pushed up. */
#define MIC_CLEAN_FLOOR_DBFS    (-50.0f)
#define MIC_CLIP_LIMIT_RATIO    (0.0005f)

esp_err_t mic_probe_gain_scan(esp_codec_dev_handle_t mic, int *recommend_out)
{
    ESP_RETURN_ON_FALSE(mic, ESP_ERR_INVALID_ARG, TAG, "microphone handle is null");

    probe_banner("3b/5  MICROPHONE GAIN SWEEP");
    probe_note("Keep the room quiet and the board still; the scan picks the");
    probe_note("highest gain that still leaves a clean noise floor.");

    int best_gain = k_gain_table[0];
    float best_floor = 0.0f;
    int fallback_gain = k_gain_table[0];
    float fallback_floor = 999.0f;
    bool found = false;

    for (size_t i = 0; i < ARRAY_SIZE(k_gain_table); i++) {
        mic_analysis_t analysis;
        if (mic_probe_measure(mic, 1, k_gain_table[i], &analysis) != ESP_OK || !analysis.valid) {
            ESP_LOGW(TAG, "  gain %2d dB : measurement failed", k_gain_table[i]);
            continue;
        }

        ESP_LOGI(TAG, "  gain %2d dB : floor %7.1f dBFS  peak %7.1f dBFS  clip %.4f%%",
                 k_gain_table[i], (double)analysis.ac_rms_dbfs, (double)analysis.peak_dbfs,
                 (double)analysis.clip_ratio * 100.0);

        if (analysis.ac_rms_dbfs < fallback_floor) {
            fallback_floor = analysis.ac_rms_dbfs;
            fallback_gain = k_gain_table[i];
        }

        if (analysis.ac_rms_dbfs <= MIC_CLEAN_FLOOR_DBFS &&
            analysis.clip_ratio <= MIC_CLIP_LIMIT_RATIO) {
            /* Table is ascending, so the last qualifying entry wins. */
            best_gain = k_gain_table[i];
            best_floor = analysis.ac_rms_dbfs;
            found = true;
        }
    }

    const int recommended = found ? best_gain : fallback_gain;
    const float recommended_floor = found ? best_floor : fallback_floor;

    if (recommend_out) {
        *recommend_out = recommended;
    }

    probe_kv("recommended gain", "%d dB (floor %.1f dBFS)", recommended, (double)recommended_floor);
    if (!found) {
        probe_note("No gain kept the floor under %.0f dBFS; picked the quietest.",
                   (double)MIC_CLEAN_FLOOR_DBFS);
        probe_note("A raised floor across all gains points at an electrical or");
        probe_note("acoustic problem, not a gain choice. Check the enclosure and");
        probe_note("whether something is physically touching the microphone.");
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Step 3 entry point
 * ---------------------------------------------------------------------- */

void mic_probe_run(int default_gain_db, esp_codec_dev_handle_t *out_mic,
                   mic_analysis_t *out, int *recommended_gain_db)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (out_mic) {
        *out_mic = NULL;
    }
    if (recommended_gain_db) {
        *recommended_gain_db = default_gain_db;
    }

    probe_banner("3/5  MICROPHONE NOISE FLOOR");
    probe_note("Stage: quiet room, board flat on the desk, nothing touching it.");
    probe_note("Capture is %d Hz / %d bit / mono, which excludes the board's", MIC_SAMPLE_RATE, MIC_BITS);
    probe_note("unused second channel that a stereo open would have returned.");

    esp_codec_dev_handle_t mic = NULL;
    const esp_err_t open_err = mic_probe_open(&mic, default_gain_db);
    if (open_err != ESP_OK) {
        probe_check("ES8311 microphone open", false, "%s", esp_err_to_name(open_err));
        return;
    }
    probe_check("ES8311 microphone open at 16 kHz mono", true, "gain %d dB", default_gain_db);

    mic_analysis_t analysis;
    if (mic_probe_measure(mic, CONFIG_HW_PROBE_MIC_CAPTURE_SEC, default_gain_db, &analysis) != ESP_OK ||
        !analysis.valid) {
        probe_check("noise floor capture", false, "no samples captured");
        return;
    }

    ESP_LOGI(TAG, "  captured %lu samples over %d s",
             (unsigned long)analysis.samples, CONFIG_HW_PROBE_MIC_CAPTURE_SEC);
    probe_kv("noise floor (AC RMS)", "%.1f dBFS", (double)analysis.ac_rms_dbfs);
    probe_kv("raw RMS (with DC)", "%.1f dBFS", (double)analysis.raw_rms_dbfs);
    probe_kv("peak", "%.1f dBFS (crest %.1f dB)", (double)analysis.peak_dbfs,
             (double)analysis.crest_db);
    probe_kv("DC offset", "%.1f LSB", (double)analysis.dc_offset);
    probe_kv("clipping", "%.4f%% (%lu samples)", (double)analysis.clip_ratio * 100.0,
             (unsigned long)analysis.clipped_samples);
    probe_kv("band <200 Hz", "%.1f dBFS  (JO-B low band)", (double)analysis.band_low_db);
    probe_kv("band 200-500 Hz", "%.1f dBFS  (JO-A mid band)", (double)analysis.band_mid_db);
    probe_kv("band 500-2000 Hz", "%.1f dBFS  (onset cue)", (double)analysis.band_high_db);

    /* Judgement thresholds. These are starting points for the encoding stage,
     * not absolute limits. */
    const bool floor_ok = analysis.ac_rms_dbfs <= -55.0f;
    const bool floor_usable = analysis.ac_rms_dbfs <= -45.0f;
    const bool dc_ok = fabsf(analysis.dc_offset) < 300.0f;

    probe_check("noise floor", floor_usable,
                floor_ok ? "clean (<= -55 dBFS)" : (floor_usable ? "usable (<= -45 dBFS)" : "high (> -45 dBFS)"));
    probe_check("DC bias in range", dc_ok, "%.1f LSB", (double)analysis.dc_offset);
    probe_check("no clipping at this gain", analysis.clip_ratio <= MIC_CLIP_LIMIT_RATIO,
                "%.4f%%", (double)analysis.clip_ratio * 100.0);

    if (out) {
        *out = analysis;
    }

    int recommended = default_gain_db;
    if (CONFIG_HW_PROBE_MIC_GAIN_SCAN) {
        mic_probe_gain_scan(mic, &recommended);

        if (recommended != default_gain_db) {
            /* Re-measure at the recommended gain and hand THAT to the caller.
             * Quoting the default-gain floor next to the recommended gain would
             * mislead the encoding constants downstream, because the floor and
             * the bands both scale with the gain they were taken at. */
            mic_analysis_t at_recommended;
            if (mic_probe_measure(mic, CONFIG_HW_PROBE_MIC_CAPTURE_SEC, recommended,
                                  &at_recommended) == ESP_OK && at_recommended.valid) {
                analysis = at_recommended;
                if (out) {
                    *out = analysis;
                }
                probe_kv("floor at recommended gain", "%.1f dBFS @ %d dB",
                         (double)analysis.ac_rms_dbfs, recommended);
                probe_kv("band <200 Hz", "%.1f dBFS", (double)analysis.band_low_db);
                probe_kv("band 200-500 Hz", "%.1f dBFS", (double)analysis.band_mid_db);
                probe_kv("band 500-2000 Hz", "%.1f dBFS", (double)analysis.band_high_db);
            } else {
                ESP_LOGW(TAG, "re-measure at %d dB failed; keeping the default-gain figures",
                         recommended);
            }
        }

        /* Leave the codec at the recommended setting so the UDP feature stream
         * sends what the recommendation is based on. */
        (void)mic_probe_set_gain(mic, recommended);
    }

    if (recommended_gain_db) {
        *recommended_gain_db = recommended;
    }
    if (out_mic) {
        *out_mic = mic;
    }
}
