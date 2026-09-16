/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-TTS SD -> PSRAM runtime validation.
 *
 * Voice data is intentionally kept out of the 16 MB flash partition table.
 * A .dat file on SD is loaded into 8 MB PSRAM and passed to
 * esp_tts_voice_set_init(), then synthesized PCM is streamed directly to
 * voice_audio / MAX98357A.
 */

#include "voice_tts.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_tts.h"
#include "esp_tts_voice_template.h"
#include "freertos/FreeRTOS.h"
#include "voice_audio.h"

static const char *TAG = "voice_tts";

#define VOICE_TTS_MAX_DATA_BYTES       (6U * 1024U * 1024U)
#define VOICE_TTS_PLAY_TIMEOUT_MS      1000U
#define VOICE_TTS_SAMPLE_RATE_HZ       16000U

static portMUX_TYPE s_tts_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_tts_busy;
static void *s_voice_data_cache;
static size_t s_voice_data_cache_size;
static char s_voice_data_cache_path[160];

static void log_heap(const char *where)
{
    ESP_LOGI(TAG,
             "%s: internal_free=%u largest=%u psram_free=%u psram_largest=%u",
             where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static bool try_lock(void)
{
    bool ok = false;
    portENTER_CRITICAL(&s_tts_mux);
    if (!s_tts_busy) {
        s_tts_busy = true;
        ok = true;
    }
    portEXIT_CRITICAL(&s_tts_mux);
    return ok;
}

static void unlock(void)
{
    portENTER_CRITICAL(&s_tts_mux);
    s_tts_busy = false;
    portEXIT_CRITICAL(&s_tts_mux);
}

static esp_err_t load_file_to_psram(const char *path, void **out_data, size_t *out_size)
{
    struct stat st = {0};
    ESP_RETURN_ON_FALSE(path && path[0] && out_data && out_size,
                        ESP_ERR_INVALID_ARG, TAG, "invalid voice-data args");
    ESP_RETURN_ON_FALSE(stat(path, &st) == 0 && S_ISREG(st.st_mode),
                        ESP_ERR_NOT_FOUND, TAG, "voice data not found: %s", path);
    ESP_RETURN_ON_FALSE(st.st_size > 0 && (uint64_t)st.st_size <= VOICE_TTS_MAX_DATA_BYTES,
                        ESP_ERR_INVALID_SIZE, TAG,
                        "voice data size invalid: %lld (max=%u)",
                        (long long)st.st_size,
                        (unsigned)VOICE_TTS_MAX_DATA_BYTES);

    size_t size = (size_t)st.st_size;
    void *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(data, ESP_ERR_NO_MEM, TAG,
                        "PSRAM voice-data allocation failed bytes=%u",
                        (unsigned)size);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        free(data);
        ESP_LOGE(TAG, "open voice data failed: %s", path);
        return ESP_FAIL;
    }

    size_t done = 0;
    while (done < size) {
        size_t got = fread((uint8_t *)data + done, 1, size - done, fp);
        if (got == 0) {
            if (ferror(fp)) {
                ESP_LOGE(TAG, "read voice data failed at %u/%u",
                         (unsigned)done, (unsigned)size);
            }
            fclose(fp);
            free(data);
            return ESP_FAIL;
        }
        done += got;
    }
    fclose(fp);

    *out_data = data;
    *out_size = size;
    ESP_LOGI(TAG, "voice data loaded to PSRAM path=%s bytes=%u",
             path, (unsigned)size);
    return ESP_OK;
}

static esp_err_t ensure_voice_data_cached(const char *path,
                                          void **out_data,
                                          size_t *out_size)
{
    ESP_RETURN_ON_FALSE(path && path[0] && out_data && out_size,
                        ESP_ERR_INVALID_ARG, TAG, "invalid voice-data cache args");

    if (s_voice_data_cache && strcmp(s_voice_data_cache_path, path) == 0) {
        *out_data = s_voice_data_cache;
        *out_size = s_voice_data_cache_size;
        ESP_LOGI(TAG, "voice data cache hit bytes=%u",
                 (unsigned)s_voice_data_cache_size);
        return ESP_OK;
    }

    if (s_voice_data_cache) {
        ESP_LOGW(TAG, "voice data path changed; replacing PSRAM cache");
        free(s_voice_data_cache);
        s_voice_data_cache = NULL;
        s_voice_data_cache_size = 0;
        s_voice_data_cache_path[0] = '\0';
    }

    void *data = NULL;
    size_t size = 0;
    esp_err_t err = load_file_to_psram(path, &data, &size);
    if (err != ESP_OK) {
        return err;
    }

    s_voice_data_cache = data;
    s_voice_data_cache_size = size;
    snprintf(s_voice_data_cache_path, sizeof(s_voice_data_cache_path), "%s", path);

    *out_data = s_voice_data_cache;
    *out_size = s_voice_data_cache_size;
    ESP_LOGI(TAG, "voice data cache ready bytes=%u psram_free=%u",
             (unsigned)size,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

esp_err_t voice_tts_preload_file(const char *voice_data_path)
{
    ESP_RETURN_ON_FALSE(voice_data_path && voice_data_path[0],
                        ESP_ERR_INVALID_ARG, TAG, "invalid voice-data path");
    ESP_RETURN_ON_FALSE(try_lock(), ESP_ERR_INVALID_STATE, TAG, "TTS busy");

    void *data = NULL;
    size_t size = 0;
    esp_err_t err = ensure_voice_data_cached(voice_data_path, &data, &size);
    unlock();
    return err;
}

esp_err_t voice_tts_speak_file(const char *voice_data_path,
                               const char *utf8_text,
                               unsigned int speed,
                               voice_tts_result_t *out_result)
{
    ESP_RETURN_ON_FALSE(voice_data_path && voice_data_path[0] &&
                        utf8_text && utf8_text[0],
                        ESP_ERR_INVALID_ARG, TAG, "invalid TTS args");
    ESP_RETURN_ON_FALSE(speed <= 5, ESP_ERR_INVALID_ARG, TAG, "speed must be 0..5");
    ESP_RETURN_ON_FALSE(try_lock(), ESP_ERR_INVALID_STATE, TAG, "TTS busy");

    esp_err_t ret = ESP_OK;
    void *voice_data = NULL;
    size_t voice_data_size = 0;
    esp_tts_voice_t *voice = NULL;
    esp_tts_handle_t tts = NULL;
    bool playback_started = false;
    uint64_t pcm_samples = 0;

    if (out_result) {
        memset(out_result, 0, sizeof(*out_result));
    }

    ESP_LOGI(TAG, "speak begin text='%s' speed=%u", utf8_text, speed);
    log_heap("before voice-data load");

    ret = ensure_voice_data_cached(voice_data_path, &voice_data, &voice_data_size);
    if (ret != ESP_OK) {
        goto cleanup;
    }
    log_heap("after voice-data load");

    /*
     * Reserve the low-DMA I2S ring before ESP-TTS creates its runtime heap.
     * This protects the two 4 KiB DMA-capable channel allocations from later
     * internal-heap fragmentation.
     */
    ret = voice_audio_playback_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "playback start failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    playback_started = true;
    log_heap("after audio playback start");

    voice = esp_tts_voice_set_init(&esp_tts_voice_template, voice_data);
    if (!voice) {
        ESP_LOGE(TAG, "esp_tts_voice_set_init failed");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    tts = esp_tts_create(voice);
    if (!tts) {
        ESP_LOGE(TAG, "esp_tts_create failed");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    log_heap("after ESP-TTS create");

    if (!esp_tts_parse_chinese(tts, utf8_text)) {
        ESP_LOGE(TAG, "esp_tts_parse_chinese failed");
        ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    for (;;) {
        int len = 0;
        short *pcm = esp_tts_stream_play(tts, &len, speed);
        if (len <= 0) {
            break;
        }
        if (!pcm) {
            ESP_LOGE(TAG, "ESP-TTS returned NULL PCM with len=%d", len);
            ret = ESP_FAIL;
            goto cleanup;
        }

        ret = voice_audio_playback_write(
            (const int16_t *)pcm, (size_t)len, VOICE_TTS_PLAY_TIMEOUT_MS);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PCM playback failed: %s", esp_err_to_name(ret));
            goto cleanup;
        }
        pcm_samples += (uint64_t)len;
    }

    ESP_LOGI(TAG, "speak complete samples=%llu ms=%u",
             (unsigned long long)pcm_samples,
             (unsigned)((pcm_samples * 1000ULL) / VOICE_TTS_SAMPLE_RATE_HZ));

cleanup:
    if (tts) {
        esp_tts_stream_reset(tts);
        esp_tts_destroy(tts);
        tts = NULL;
    }
    if (voice) {
        esp_tts_voice_set_free(voice);
        voice = NULL;
    }
    if (playback_started) {
        esp_err_t stop_err = voice_audio_playback_stop();
        if (ret == ESP_OK && stop_err != ESP_OK) {
            ret = stop_err;
        }
    }

    /*
     * TTS test is not the listening loop. Release I2S DMA after speaking so
     * WakeNet can reclaim the internal RAM on the next Voice Service start.
     */
    voice_audio_info_t info = {0};
    voice_audio_get_info(&info);
    if (info.initialized && info.state == VOICE_AUDIO_STATE_IDLE) {
        (void)voice_audio_deinit();
    }

    /* Voice data remains cached in PSRAM for the next reply. */
    voice_data = NULL;
    log_heap("after TTS cleanup (voice cache retained)");

    if (out_result) {
        out_result->voice_data_bytes = voice_data_size;
        out_result->pcm_samples = pcm_samples;
        out_result->pcm_ms =
            (uint32_t)((pcm_samples * 1000ULL) / VOICE_TTS_SAMPLE_RATE_HZ);
        out_result->internal_free_after =
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        out_result->psram_free_after =
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }

    unlock();
    return ret;
}
