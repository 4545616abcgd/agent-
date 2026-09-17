/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-Claw Voice Service V2.7 product capture state machine
 *
 * Deliberately small first integration:
 *   - direct WakeNet interface (no AFE task pair)
 *   - one microphone, 16 kHz signed PCM16 from voice_audio
 *   - local VAD turn boundaries with a 10 s no-speech window
 *   - utterance stored in PSRAM, never written to SD
 *   - completed utterance handoff to the product dialog pipeline
 *   - local echo retained only as an engineering fallback when no sink is registered
 */

#include "voice_service.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model_path.h"
#include "voice_audio.h"

static const char *TAG = "voice_service";

#define VOICE_SERVICE_TASK_STACK_BYTES       3584U
#define VOICE_SERVICE_TTS_ONLY_ISOLATE       0
#define VOICE_SERVICE_TASK_PRIORITY          5
#define VOICE_SERVICE_TASK_CORE              1
#define VOICE_SERVICE_READ_TIMEOUT_MS        1000U

#define VOICE_SERVICE_MAX_UTTERANCE_MS      20000U
#define VOICE_SERVICE_WAIT_SPEECH_MS        10000U
#define VOICE_SERVICE_CAPTURE_BUFFER_MS       \
    (VOICE_SERVICE_WAIT_SPEECH_MS + VOICE_SERVICE_MAX_UTTERANCE_MS)
#define VOICE_SERVICE_WAKE_GUARD_MS          128U
#define VOICE_SERVICE_END_SILENCE_MS        1000U
#define VOICE_SERVICE_PRE_ROLL_MS            200U
#define VOICE_SERVICE_POST_ROLL_MS           160U
#define VOICE_SERVICE_WAKE_REARM_MS          1000U
#define VOICE_SERVICE_ACK_PROMPT_TEXT         "我在"
#define VOICE_SERVICE_ACK_TONE1_HZ           880U
#define VOICE_SERVICE_ACK_TONE1_MS          150U
#define VOICE_SERVICE_ACK_GAP_MS             40U
#define VOICE_SERVICE_ACK_TONE2_HZ           1175U
#define VOICE_SERVICE_ACK_TONE2_MS          180U
#define VOICE_SERVICE_ACK_AMPLITUDE         15000

#define VOICE_SERVICE_VAD_MIN_START_RMS      120U
#define VOICE_SERVICE_VAD_MIN_END_RMS        80U
#define VOICE_SERVICE_VAD_START_NOISE_X10    25U   /* 2.5 x noise floor */
#define VOICE_SERVICE_VAD_END_NOISE_X10      18U   /* 1.8 x noise floor */
#define VOICE_SERVICE_VAD_SPEECH_FRAMES      2U
#define VOICE_SERVICE_VAD_CONFIRM_MS         192U

#define VOICE_SERVICE_MODEL_PARTITION        "model"
#define VOICE_SERVICE_MODEL_KEYWORD          "nihaoxiaoyi"
#define VOICE_SERVICE_FOLLOWUP_RATE_HZ       16000U
#define VOICE_SERVICE_FOLLOWUP_FRAME_SAMPLES  512U

typedef struct {
    volatile bool running;
    volatile bool stop_requested;
    volatile voice_service_state_t state;
    volatile esp_err_t last_error;
    bool followup_mode;
    TaskHandle_t task;

    srmodel_list_t *models;
    const esp_wn_iface_t *wakenet;
    model_iface_data_t *model_data;

    int16_t *frame;
    int16_t *utterance;
    size_t frame_samples;
    size_t utterance_capacity_samples;
    uint32_t sample_rate_hz;

    uint32_t wake_count;
    uint32_t utterance_count;
    uint32_t noise_rms;
    uint32_t last_rms;
    uint32_t last_peak;
    uint32_t last_utterance_ms;
    uint32_t task_stack_high_water_bytes;

    char model_name[64];
    char wake_word[64];
} voice_service_ctx_t;

static voice_service_ctx_t s_voice;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static voice_service_utterance_sink_fn s_utterance_sink;
static void *s_utterance_sink_ctx;
static voice_service_realtime_sink_t s_realtime_sink;
static void *s_realtime_sink_ctx;

extern const uint8_t s_wake_ack_pcm_start[]
    asm("_binary_wake_ack_wozai_pcm_start");
extern const uint8_t s_wake_ack_pcm_end[]
    asm("_binary_wake_ack_wozai_pcm_end");

static uint32_t u32_max(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

static uint32_t ms_to_samples(uint32_t ms, uint32_t sample_rate)
{
    return (uint32_t)(((uint64_t)ms * sample_rate) / 1000U);
}

static uint32_t samples_to_ms(size_t samples, uint32_t sample_rate)
{
    if (sample_rate == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)samples * 1000U) / sample_rate);
}

static void calc_frame_level(const int16_t *samples,
                             size_t count,
                             uint32_t *out_rms,
                             uint32_t *out_peak)
{
    uint64_t sum_sq = 0;
    uint32_t peak = 0;

    for (size_t i = 0; i < count; ++i) {
        int32_t s = samples[i];
        uint32_t abs_s = (uint32_t)(s < 0 ? -s : s);
        if (abs_s > peak) {
            peak = abs_s;
        }
        sum_sq += (uint64_t)((int64_t)s * (int64_t)s);
    }

    uint32_t rms = 0;
    if (count > 0) {
        rms = (uint32_t)sqrt((double)sum_sq / (double)count);
    }
    if (out_rms) {
        *out_rms = rms;
    }
    if (out_peak) {
        *out_peak = peak;
    }
}

static void status_set_state(voice_service_state_t state)
{
    portENTER_CRITICAL(&s_mux);
    s_voice.state = state;
    portEXIT_CRITICAL(&s_mux);
}

static void status_set_error(esp_err_t err)
{
    portENTER_CRITICAL(&s_mux);
    s_voice.last_error = err;
    s_voice.state = VOICE_SERVICE_STATE_ERROR;
    portEXIT_CRITICAL(&s_mux);
}

static void update_level_status(uint32_t rms, uint32_t peak)
{
    portENTER_CRITICAL(&s_mux);
    s_voice.last_rms = rms;
    s_voice.last_peak = peak;
    portEXIT_CRITICAL(&s_mux);
}

static void update_noise_floor(uint32_t rms)
{
    portENTER_CRITICAL(&s_mux);
    if (s_voice.noise_rms == 0) {
        s_voice.noise_rms = rms > 0 ? rms : 1;
    } else {
        /*
         * Asymmetric EMA:
         * - normal/quiet frames track at 1/32
         * - strong speech-like transients may rise only very slowly (1/128)
         *
         * This prevents a 1 s wake phrase from inflating the frozen VAD
         * baseline from ~150 RMS into the thousands.
         */
        uint32_t current = s_voice.noise_rms;
        uint32_t twice = current > (UINT32_MAX / 2U) ? UINT32_MAX : current * 2U;
        if (rms <= twice) {
            s_voice.noise_rms = (current * 31U + rms) / 32U;
        } else {
            s_voice.noise_rms = (current * 127U + twice) / 128U;
        }
        if (s_voice.noise_rms == 0) {
            s_voice.noise_rms = 1;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

static bool stop_requested(void)
{
    bool stop;
    portENTER_CRITICAL(&s_mux);
    stop = s_voice.stop_requested;
    portEXIT_CRITICAL(&s_mux);
    return stop;
}

static void log_heap(const char *where)
{
    ESP_LOGI(TAG,
             "%s: internal_free=%u largest=%u psram_free=%u",
             where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void release_wakenet_only(void)
{
    if (s_voice.model_data && s_voice.wakenet) {
        s_voice.wakenet->destroy(s_voice.model_data);
        s_voice.model_data = NULL;
    }
    if (s_voice.models) {
        esp_srmodel_deinit(s_voice.models);
        s_voice.models = NULL;
    }
    s_voice.wakenet = NULL;
    ESP_LOGI(TAG, "WakeNet resources released for realtime transport");
}

static void release_sr_resources(void)
{
    if (s_voice.model_data && s_voice.wakenet) {
        s_voice.wakenet->destroy(s_voice.model_data);
        s_voice.model_data = NULL;
    }
    if (s_voice.models) {
        esp_srmodel_deinit(s_voice.models);
        s_voice.models = NULL;
    }
    s_voice.wakenet = NULL;

    free(s_voice.frame);
    s_voice.frame = NULL;
    free(s_voice.utterance);
    s_voice.utterance = NULL;
    s_voice.frame_samples = 0;
    s_voice.utterance_capacity_samples = 0;
    s_voice.sample_rate_hz = 0;
}

static esp_err_t init_sr_resources(void)
{
    log_heap("before WakeNet init");

    s_voice.models = esp_srmodel_init(VOICE_SERVICE_MODEL_PARTITION);
    if (!s_voice.models) {
        ESP_LOGE(TAG, "esp_srmodel_init(%s) failed", VOICE_SERVICE_MODEL_PARTITION);
        return ESP_ERR_NOT_FOUND;
    }

    char *model_name = esp_srmodel_filter(
        s_voice.models, ESP_WN_PREFIX, VOICE_SERVICE_MODEL_KEYWORD);
    if (!model_name) {
        ESP_LOGE(TAG, "No WakeNet model containing '%s' in model partition",
                 VOICE_SERVICE_MODEL_KEYWORD);
        return ESP_ERR_NOT_FOUND;
    }

    s_voice.wakenet = esp_wn_handle_from_name(model_name);
    if (!s_voice.wakenet) {
        ESP_LOGE(TAG, "No WakeNet interface for model %s", model_name);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* DET_MODE_95 keeps coefficients in PSRAM on current ESP-SR and is the
     * safer choice for this ESP-Claw build's very tight internal heap. */
    s_voice.model_data = s_voice.wakenet->create(model_name, DET_MODE_95);
    if (!s_voice.model_data) {
        ESP_LOGE(TAG, "WakeNet create failed for %s", model_name);
        return ESP_ERR_NO_MEM;
    }

    int sample_rate = s_voice.wakenet->get_samp_rate(s_voice.model_data);
    int frame_samples = s_voice.wakenet->get_samp_chunksize(s_voice.model_data);
    int channels = s_voice.wakenet->get_channel_num(s_voice.model_data);
    if (sample_rate != 16000 || frame_samples <= 0 || channels != 1) {
        ESP_LOGE(TAG,
                 "Unsupported WakeNet format: rate=%d frame=%d channels=%d",
                 sample_rate, frame_samples, channels);
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_voice.sample_rate_hz = (uint32_t)sample_rate;
    s_voice.frame_samples = (size_t)frame_samples;
    s_voice.utterance_capacity_samples =
        ms_to_samples(VOICE_SERVICE_CAPTURE_BUFFER_MS, s_voice.sample_rate_hz);

    s_voice.frame = heap_caps_malloc(
        s_voice.frame_samples * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_voice.utterance = heap_caps_malloc(
        s_voice.utterance_capacity_samples * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_voice.frame || !s_voice.utterance) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM voice buffers");
        return ESP_ERR_NO_MEM;
    }

    strlcpy(s_voice.model_name, model_name, sizeof(s_voice.model_name));
    char *wake_words = esp_srmodel_get_wake_words(s_voice.models, model_name);
    strlcpy(s_voice.wake_word,
            wake_words ? wake_words : "你好小智",
            sizeof(s_voice.wake_word));

    ESP_LOGI(TAG,
             "WakeNet ready model=%s wake=%s frame=%u rate=%u mode=95",
             s_voice.model_name,
             s_voice.wake_word,
             (unsigned)s_voice.frame_samples,
             (unsigned)s_voice.sample_rate_hz);
    log_heap("after WakeNet init");
    return ESP_OK;
}

static esp_err_t init_followup_resources(void)
{
    s_voice.sample_rate_hz = VOICE_SERVICE_FOLLOWUP_RATE_HZ;
    s_voice.frame_samples = VOICE_SERVICE_FOLLOWUP_FRAME_SAMPLES;
    s_voice.utterance_capacity_samples =
        ms_to_samples(VOICE_SERVICE_CAPTURE_BUFFER_MS, s_voice.sample_rate_hz);

    s_voice.frame = heap_caps_malloc(
        s_voice.frame_samples * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_voice.utterance = heap_caps_malloc(
        s_voice.utterance_capacity_samples * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_voice.frame || !s_voice.utterance) {
        ESP_LOGE(TAG, "Failed to allocate follow-up capture buffers in PSRAM");
        return ESP_ERR_NO_MEM;
    }

    s_voice.model_name[0] = '\0';
    s_voice.wake_word[0] = '\0';
    ESP_LOGI(TAG, "follow-up capture ready frame=%u rate=%u (WakeNet not loaded)",
             (unsigned)s_voice.frame_samples, (unsigned)s_voice.sample_rate_hz);
    return ESP_OK;
}

static esp_err_t playback_tone(uint32_t frequency_hz, uint32_t duration_ms)
{
    if (frequency_hz == 0 || duration_ms == 0 || s_voice.sample_rate_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t total_samples =
        (size_t)(((uint64_t)s_voice.sample_rate_hz * duration_ms) / 1000U);
    const uint32_t phase_step =
        (uint32_t)(((uint64_t)frequency_hz << 32) / s_voice.sample_rate_hz);
    uint32_t phase = 0;
    size_t done = 0;
    int32_t generated_peak = 0;

    while (done < total_samples && !stop_requested()) {
        size_t count = total_samples - done;
        if (count > s_voice.frame_samples) {
            count = s_voice.frame_samples;
        }

        for (size_t i = 0; i < count; ++i) {
            /*
             * Small triangle wave: cheap to generate, no extra audio asset/RAM.
             * Add a short edge ramp to avoid a hard Class-D click.
             */
            uint32_t p = phase >> 16;
            int32_t tri = (p < 32768U) ?
                          ((int32_t)p * 2 - 32768) :
                          (98303 - (int32_t)p * 2);
            int32_t sample = (tri * VOICE_SERVICE_ACK_AMPLITUDE) / 32768;

            size_t absolute = done + i;
            const size_t ramp_samples = s_voice.sample_rate_hz / 200U; /* 5 ms */
            if (ramp_samples > 0 && absolute < ramp_samples) {
                sample = (sample * (int32_t)absolute) / (int32_t)ramp_samples;
            }
            size_t left = total_samples - absolute;
            if (ramp_samples > 0 && left < ramp_samples) {
                sample = (sample * (int32_t)left) / (int32_t)ramp_samples;
            }

            s_voice.frame[i] = (int16_t)sample;
            int32_t magnitude = sample < 0 ? -sample : sample;
            if (magnitude > generated_peak) {
                generated_peak = magnitude;
            }
            phase += phase_step;
        }

        ESP_RETURN_ON_ERROR(
            voice_audio_playback_write(s_voice.frame, count, VOICE_SERVICE_READ_TIMEOUT_MS),
            TAG, "ack tone write failed");
        done += count;
    }

    ESP_LOGI(TAG,
             "listen cue tone queued freq=%uHz duration=%ums samples=%u peak=%d i2s_bytes=%u",
             (unsigned)frequency_hz,
             (unsigned)duration_ms,
             (unsigned)done,
             (int)generated_peak,
             (unsigned)(done * 2U * sizeof(int32_t)));

    return ESP_OK;
}

static esp_err_t playback_silence(uint32_t duration_ms)
{
    const size_t total_samples =
        (size_t)(((uint64_t)s_voice.sample_rate_hz * duration_ms) / 1000U);
    size_t done = 0;

    while (done < total_samples && !stop_requested()) {
        size_t count = total_samples - done;
        if (count > s_voice.frame_samples) {
            count = s_voice.frame_samples;
        }
        memset(s_voice.frame, 0, count * sizeof(int16_t));
        ESP_RETURN_ON_ERROR(
            voice_audio_playback_write(s_voice.frame, count, VOICE_SERVICE_READ_TIMEOUT_MS),
            TAG, "ack silence write failed");
        done += count;
    }
    return ESP_OK;
}

static esp_err_t playback_wake_ack_prompt(void)
{
    size_t pcm_bytes = (size_t)(s_wake_ack_pcm_end - s_wake_ack_pcm_start);
    if (pcm_bytes == 0 || (pcm_bytes % sizeof(int16_t)) != 0) {
        ESP_LOGE(TAG, "wake ack PCM invalid bytes=%u", (unsigned)pcm_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    const int16_t *pcm = (const int16_t *)(const void *)s_wake_ack_pcm_start;
    size_t total_samples = pcm_bytes / sizeof(int16_t);
    size_t done = 0;

    while (done < total_samples && !stop_requested()) {
        size_t count = total_samples - done;
        if (count > s_voice.frame_samples) {
            count = s_voice.frame_samples;
        }
        ESP_RETURN_ON_ERROR(
            voice_audio_playback_write(pcm + done,
                                       count,
                                       VOICE_SERVICE_READ_TIMEOUT_MS),
            TAG,
            "wake ack PCM write failed");
        done += count;
    }

    ESP_LOGI(TAG,
             "listen cue voice queued text='%s' samples=%u duration=%ums",
             VOICE_SERVICE_ACK_PROMPT_TEXT,
             (unsigned)done,
             (unsigned)samples_to_ms(done, s_voice.sample_rate_hz));
    return done == total_samples ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t play_wake_ack(void)
{
    ESP_LOGI(TAG,
             "WAKE acknowledged: playing listen voice='%s'",
             VOICE_SERVICE_ACK_PROMPT_TEXT);

    voice_audio_info_t audio_info = {0};
    voice_audio_get_info(&audio_info);
    if (audio_info.state == VOICE_AUDIO_STATE_CAPTURE) {
        ESP_RETURN_ON_ERROR(
            voice_audio_capture_stop(), TAG, "capture stop before wake ack failed");
    } else if (audio_info.state != VOICE_AUDIO_STATE_IDLE) {
        ESP_LOGE(TAG, "wake ack requires idle/capture audio state, got=%d", (int)audio_info.state);
        return ESP_ERR_INVALID_STATE;
    }
    status_set_state(VOICE_SERVICE_STATE_PLAYBACK);

    esp_err_t err = voice_audio_playback_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wake ack playback start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = playback_wake_ack_prompt();
    if (err != ESP_OK && !stop_requested()) {
        ESP_LOGW(TAG,
                 "listen voice failed: %s; falling back to two-tone cue",
                 esp_err_to_name(err));
        err = playback_tone(VOICE_SERVICE_ACK_TONE1_HZ, VOICE_SERVICE_ACK_TONE1_MS);
        if (err == ESP_OK) {
            err = playback_silence(VOICE_SERVICE_ACK_GAP_MS);
        }
        if (err == ESP_OK) {
            err = playback_tone(VOICE_SERVICE_ACK_TONE2_HZ, VOICE_SERVICE_ACK_TONE2_MS);
        }
    }

    esp_err_t stop_err = voice_audio_playback_stop();
    if (err == ESP_OK && stop_err != ESP_OK) {
        err = stop_err;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (stop_requested()) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(
        voice_audio_capture_start(), TAG, "capture restart after wake ack failed");

    ESP_LOGI(TAG, "WAKE ack complete: speak command now");
    return ESP_OK;
}

static esp_err_t handoff_utterance(size_t begin_sample, size_t end_sample)
{
    if (end_sample <= begin_sample) {
        return ESP_ERR_INVALID_SIZE;
    }

    voice_service_utterance_sink_fn sink = NULL;
    void *sink_ctx = NULL;
    portENTER_CRITICAL(&s_mux);
    sink = s_utterance_sink;
    sink_ctx = s_utterance_sink_ctx;
    portEXIT_CRITICAL(&s_mux);

    if (!sink) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const size_t sample_count = end_sample - begin_sample;
    const uint32_t sample_rate_hz = s_voice.sample_rate_hz;
    int16_t *pcm = heap_caps_malloc(sample_count * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(pcm,
           s_voice.utterance + begin_sample,
           sample_count * sizeof(int16_t));

    /*
     * ASR + Root Agent + TTS need the memory currently held by WakeNet/I2S.
     * Tear those resources down before the transient dialog worker is created.
     */
    esp_err_t err = voice_audio_capture_stop();
    if (err != ESP_OK) {
        free(pcm);
        return err;
    }

    voice_audio_info_t audio_info = {0};
    voice_audio_get_info(&audio_info);
    if (audio_info.initialized && audio_info.state == VOICE_AUDIO_STATE_IDLE) {
        (void)voice_audio_deinit();
    }

    release_sr_resources();
    log_heap("before utterance handoff");

    err = sink(pcm, sample_count, sample_rate_hz, sink_ctx);
    if (err != ESP_OK) {
        free(pcm);
        return err;
    }

    ESP_LOGI(TAG,
             "utterance handed off samples=%u ms=%u",
             (unsigned)sample_count,
             (unsigned)samples_to_ms(sample_count, sample_rate_hz));
    return ESP_OK;
}

static esp_err_t echo_utterance(size_t begin_sample, size_t end_sample)
{
    if (end_sample <= begin_sample) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_RETURN_ON_ERROR(voice_audio_capture_stop(), TAG, "capture stop before echo failed");
    status_set_state(VOICE_SERVICE_STATE_PLAYBACK);

    esp_err_t err = voice_audio_playback_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "playback start failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t pos = begin_sample;
    while (pos < end_sample && !stop_requested()) {
        size_t count = end_sample - pos;
        if (count > s_voice.frame_samples) {
            count = s_voice.frame_samples;
        }
        err = voice_audio_playback_write(
            s_voice.utterance + pos, count, VOICE_SERVICE_READ_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "echo write failed: %s", esp_err_to_name(err));
            break;
        }
        pos += count;
    }

    esp_err_t stop_err = voice_audio_playback_stop();
    if (err == ESP_OK && stop_err != ESP_OK) {
        err = stop_err;
    }
    if (err != ESP_OK || stop_requested()) {
        return err;
    }

    ESP_RETURN_ON_ERROR(voice_audio_capture_start(), TAG, "capture restart after echo failed");
    status_set_state(VOICE_SERVICE_STATE_LISTENING);
    return ESP_OK;
}

static void voice_service_task(void *arg)
{
    (void)arg;

    bool realtime_started = false;
    voice_service_realtime_sink_t realtime_sink = {0};
    void *realtime_ctx = NULL;
    bool followup_mode = false;

    portENTER_CRITICAL(&s_mux);
    followup_mode = s_voice.followup_mode;
    realtime_sink = s_realtime_sink;
    realtime_ctx = s_realtime_sink_ctx;
    portEXIT_CRITICAL(&s_mux);

    if (followup_mode &&
        (!realtime_sink.pcm || !realtime_sink.end || !realtime_sink.close)) {
        ESP_LOGE(TAG, "follow-up requested without a complete realtime sink");
        status_set_error(ESP_ERR_INVALID_STATE);
        goto exit_task;
    }

    /*
     * Reserve the I2S DMA rings before WakeNet fragments internal RAM.
     * The 2026-09-12 field log showed largest_internal dropping from ~7 KB
     * to ~2 KB after WakeNet, which made the second (RX) DMA ring fail.
     * voice_audio_init() allocates the DMA descriptors/buffers but does not
     * enable the channels, so it is safe to reserve them first.
     */
    esp_err_t err = voice_audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio DMA reservation failed: %s", esp_err_to_name(err));
        status_set_error(err);
        goto exit_task;
    }

    err = followup_mode ? init_followup_resources() : init_sr_resources();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s init failed after audio reservation: %s",
                 followup_mode ? "follow-up capture" : "WakeNet",
                 esp_err_to_name(err));
        status_set_error(err);
        goto exit_task;
    }

    if (followup_mode) {
        ESP_LOGI(TAG, "follow-up ready; playing listen cue before capture");
        err = play_wake_ack();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "follow-up listen cue failed: %s", esp_err_to_name(err));
            status_set_error(err);
            goto exit_task;
        }
    } else {
        err = voice_audio_capture_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "capture start failed: %s", esp_err_to_name(err));
            status_set_error(err);
            goto exit_task;
        }
    }

    if (followup_mode) {
        status_set_state(VOICE_SERVICE_STATE_RECORDING);
        ESP_LOGI(TAG, "FOLLOW-UP LISTENING: no wake word required, timeout=%ums",
                 (unsigned)VOICE_SERVICE_WAIT_SPEECH_MS);
    } else {
        status_set_state(VOICE_SERVICE_STATE_LISTENING);
        ESP_LOGI(TAG, "LISTENING: say '%s'",
                 s_voice.wake_word[0] ? s_voice.wake_word : "Hi,ESP");
    }

    const uint32_t frame_ms =
        (uint32_t)(((uint64_t)s_voice.frame_samples * 1000U) / s_voice.sample_rate_hz);
    const uint32_t guard_frames =
        u32_max(1U, (VOICE_SERVICE_WAKE_GUARD_MS + frame_ms - 1U) / frame_ms);
    const uint32_t wait_frames =
        u32_max(1U, (VOICE_SERVICE_WAIT_SPEECH_MS + frame_ms - 1U) / frame_ms);
    const uint32_t silence_frames_needed =
        u32_max(1U, (VOICE_SERVICE_END_SILENCE_MS + frame_ms - 1U) / frame_ms);
    const size_t max_speech_samples =
        ms_to_samples(VOICE_SERVICE_MAX_UTTERANCE_MS, s_voice.sample_rate_hz);
    const uint32_t rearm_frames_needed =
        u32_max(1U, (VOICE_SERVICE_WAKE_REARM_MS + frame_ms - 1U) / frame_ms);

    bool recording = followup_mode;
    bool speech_started = false;
    bool speech_confirmed = false;
    uint32_t frames_since_wake = 0;
    uint32_t speech_frames = 0;
    uint32_t silence_frames = 0;
    size_t utterance_samples = 0;
    size_t speech_start_sample = 0;
    size_t last_speech_sample = 0;
    size_t realtime_streamed_samples = 0;
    uint32_t frozen_noise = 1;
    uint32_t rearm_frames = 0;
    uint32_t frames_total = 0;
    if (followup_mode) {
        portENTER_CRITICAL(&s_mux);
        frozen_noise = s_voice.noise_rms > 0 ? s_voice.noise_rms :
                       VOICE_SERVICE_VAD_MIN_START_RMS;
        portEXIT_CRITICAL(&s_mux);
        realtime_started = true;
    }

    while (!stop_requested()) {
        size_t got = 0;
        err = voice_audio_capture_read(
            s_voice.frame,
            s_voice.frame_samples,
            &got,
            VOICE_SERVICE_READ_TIMEOUT_MS);
        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (err != ESP_OK || got != s_voice.frame_samples) {
            ESP_LOGE(TAG,
                     "capture read failed err=%s got=%u/%u",
                     esp_err_to_name(err),
                     (unsigned)got,
                     (unsigned)s_voice.frame_samples);
            status_set_error(err != ESP_OK ? err : ESP_FAIL);
            break;
        }

        uint32_t rms = 0;
        uint32_t peak = 0;
        calc_frame_level(s_voice.frame, got, &rms, &peak);
        update_level_status(rms, peak);
        frames_total++;

        if (!recording) {
            uint32_t noise_before = 1;
            portENTER_CRITICAL(&s_mux);
            noise_before = s_voice.noise_rms > 0 ? s_voice.noise_rms : 1;
            portEXIT_CRITICAL(&s_mux);

            /*
             * A realtime turn frees WakeNet before the TLS/WebSocket work.
             * If that session dies mid-turn, or the command window times out
             * before any speech, we land back here without a detector, so
             * rebuild it instead of dereferencing a NULL wakenet.
             */
            if (!s_voice.wakenet) {
                ESP_LOGW(TAG, "WakeNet absent after realtime turn; rebuilding detector");
                release_sr_resources();
                err = init_sr_resources();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "WakeNet rebuild failed: %s", esp_err_to_name(err));
                    status_set_error(err);
                    goto exit_task;
                }
                /* s_voice.frame was reallocated; drop this stale sample. */
                continue;
            }

            wakenet_state_t detected = s_voice.wakenet->detect(s_voice.model_data, s_voice.frame);

            if (rearm_frames > 0) {
                rearm_frames--;
                if (rearm_frames == 0) {
                    ESP_LOGI(TAG, "WakeNet re-armed");
                }
            } else if (detected == WAKENET_DETECTED) {
                portENTER_CRITICAL(&s_mux);
                s_voice.wake_count++;
                frozen_noise = noise_before;
                portEXIT_CRITICAL(&s_mux);

                ESP_LOGW(TAG,
                         "WAKE detected word=%s noise_rms=%u",
                         s_voice.wake_word[0] ? s_voice.wake_word : "Hi,ESP",
                         (unsigned)frozen_noise);

                if (realtime_sink.begin && realtime_sink.pcm && realtime_sink.end) {
                    /*
                     * V2.5.1 architecture fix:
                     *   wake -> stop capture -> free WakeNet -> establish cloud
                     *   session -> only then play the "speak now" beep.
                     *
                     * In V2.5.0 the beep/capture started ~0.9 s before
                     * session.created, creating an unavoidable uplink backlog.
                     */
                    err = voice_audio_capture_stop();
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "capture stop before realtime connect failed: %s",
                                 esp_err_to_name(err));
                        status_set_error(err);
                        goto exit_task;
                    }

                    release_wakenet_only();
                    ESP_LOGI(TAG, "Connecting realtime cloud before listen beep");
                    esp_err_t rt_err = realtime_sink.begin(s_voice.sample_rate_hz, realtime_ctx);
                    if (rt_err == ESP_OK) {
                        realtime_started = true;
#if VOICE_SERVICE_TTS_ONLY_ISOLATE
                        /* V2.5.4: isolate the cloud->ESP audio path.  Keep the
                         * familiar local beep, but deliberately send ZERO mic
                         * PCM afterwards.  realtime_sink.end() tells the duplex
                         * backend to run its text-only TTS probe. */
                        ESP_LOGI(TAG, "V2.5.4 TTS-only isolate: cloud ready; beep then skip microphone capture");
                        err = play_wake_ack();
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "TTS-only wake acknowledgement failed: %s", esp_err_to_name(err));
                            if (realtime_sink.abort) realtime_sink.abort(realtime_ctx);
                            realtime_started = false;
                            status_set_error(err);
                            goto exit_task;
                        }

                        voice_audio_info_t isolate_audio_info = {0};
                        voice_audio_get_info(&isolate_audio_info);
                        if (isolate_audio_info.initialized && isolate_audio_info.state == VOICE_AUDIO_STATE_IDLE) {
                            (void)voice_audio_deinit();
                        }
                        release_sr_resources();
                        rt_err = realtime_sink.end(realtime_ctx);
                        if (rt_err != ESP_OK) {
                            ESP_LOGE(TAG, "V2.5.4 TTS-only probe request failed: %s", esp_err_to_name(rt_err));
                            if (realtime_sink.abort) realtime_sink.abort(realtime_ctx);
                            status_set_error(rt_err);
                        } else {
                            ESP_LOGI(TAG, "V2.5.4 TTS-only probe requested; zero microphone PCM sent");
                        }
                        realtime_started = false;
                        goto exit_task;
#else
                        ESP_LOGI(TAG, "V2.8.2 OFFICIAL COMMIT: cloud ready; playing listen voice");
#endif
                    } else {
                        ESP_LOGE(TAG, "Realtime voice agent begin failed: %s",
                                 esp_err_to_name(rt_err));
                        status_set_error(rt_err);
                        goto exit_task;
                    }
                }

                err = play_wake_ack();
                if (err != ESP_OK) {
                    if (!stop_requested()) {
                        ESP_LOGE(TAG, "wake acknowledgement failed: %s", esp_err_to_name(err));
                        status_set_error(err);
                    }
                    if (realtime_started && realtime_sink.abort) {
                        realtime_sink.abort(realtime_ctx);
                        realtime_started = false;
                    }
                    goto exit_task;
                }

                recording = true;
                speech_started = false;
                speech_confirmed = false;
                frames_since_wake = 0;
                speech_frames = 0;
                silence_frames = 0;
                utterance_samples = 0;
                speech_start_sample = 0;
                last_speech_sample = 0;
                realtime_streamed_samples = 0;
                status_set_state(VOICE_SERVICE_STATE_RECORDING);

                ESP_LOGI(TAG,
                         "RECORDING: V2.8.2 CONFIRMED VAD wait=%ums confirm=%ums end_silence=%ums max_speech=%ums",
                         (unsigned)VOICE_SERVICE_WAIT_SPEECH_MS,
                         (unsigned)VOICE_SERVICE_VAD_CONFIRM_MS,
                         (unsigned)VOICE_SERVICE_END_SILENCE_MS,
                         (unsigned)VOICE_SERVICE_MAX_UTTERANCE_MS);
            } else {
                update_noise_floor(rms);
            }
        } else {
            size_t room = s_voice.utterance_capacity_samples - utterance_samples;
            size_t copy_count = got < room ? got : room;
            if (copy_count > 0) {
                memcpy(s_voice.utterance + utterance_samples,
                       s_voice.frame,
                       copy_count * sizeof(int16_t));
                utterance_samples += copy_count;
            }
            frames_since_wake++;

            uint32_t start_rms = u32_max(
                VOICE_SERVICE_VAD_MIN_START_RMS,
                (frozen_noise * VOICE_SERVICE_VAD_START_NOISE_X10) / 10U);
            uint32_t end_rms = u32_max(
                VOICE_SERVICE_VAD_MIN_END_RMS,
                (frozen_noise * VOICE_SERVICE_VAD_END_NOISE_X10) / 10U);

            bool start_voice = (rms >= start_rms) || (peak >= start_rms * 6U);
            bool keep_voice = (rms >= end_rms) || (peak >= end_rms * 6U);

            if (frames_since_wake > guard_frames) {
                if (!speech_started) {
                    if (start_voice) {
                        speech_frames++;
                    } else {
                        speech_frames = 0;
                    }
                    if (speech_frames >= VOICE_SERVICE_VAD_SPEECH_FRAMES) {
                        speech_started = true;
                        size_t back = got * VOICE_SERVICE_VAD_SPEECH_FRAMES;
                        speech_start_sample = utterance_samples > back ?
                                              utterance_samples - back : 0;
                        last_speech_sample = utterance_samples;
                        silence_frames = 0;
                        ESP_LOGI(TAG,
                                 "VAD speech start rms=%u peak=%u start_th=%u end_th=%u",
                                 (unsigned)rms,
                                 (unsigned)peak,
                                 (unsigned)start_rms,
                                 (unsigned)end_rms);
                    }
                } else {
                    if (keep_voice) {
                        last_speech_sample = utterance_samples;
                        silence_frames = 0;
                    } else {
                        silence_frames++;
                    }
                }
            }

            bool end_silence = speech_started && silence_frames >= silence_frames_needed;
            size_t speech_span_samples = utterance_samples > speech_start_sample ?
                                         utterance_samples - speech_start_sample : 0;
            size_t active_speech_samples = last_speech_sample > speech_start_sample ?
                                           last_speech_sample - speech_start_sample : 0;
            size_t confirm_samples =
                ms_to_samples(VOICE_SERVICE_VAD_CONFIRM_MS, s_voice.sample_rate_hz);
            bool just_confirmed = false;
            if (speech_started && !speech_confirmed &&
                active_speech_samples >= confirm_samples) {
                speech_confirmed = true;
                just_confirmed = true;
                ESP_LOGI(TAG, "VAD speech confirmed active_ms=%u",
                         (unsigned)samples_to_ms(active_speech_samples,
                                                 s_voice.sample_rate_hz));
            }

            if (realtime_started && realtime_sink.pcm && speech_confirmed) {
                size_t stream_begin = realtime_streamed_samples;
                if (just_confirmed) {
                    size_t pre_roll =
                        ms_to_samples(VOICE_SERVICE_PRE_ROLL_MS, s_voice.sample_rate_hz);
                    stream_begin = speech_start_sample > pre_roll ?
                                   speech_start_sample - pre_roll : 0;
                }
                while (stream_begin < utterance_samples) {
                    size_t stream_count = utterance_samples - stream_begin;
                    if (stream_count > s_voice.frame_samples) {
                        stream_count = s_voice.frame_samples;
                    }
                    esp_err_t rt_err = realtime_sink.pcm(
                        s_voice.utterance + stream_begin,
                        stream_count,
                        s_voice.sample_rate_hz,
                        realtime_ctx);
                    if (rt_err != ESP_OK) {
                        ESP_LOGE(TAG, "Realtime PCM push failed: %s; aborting this turn",
                                 esp_err_to_name(rt_err));
                        if (realtime_sink.abort) realtime_sink.abort(realtime_ctx);
                        realtime_started = false;
                        status_set_error(rt_err);
                        goto exit_task;
                    }
                    stream_begin += stream_count;
                }
                realtime_streamed_samples = utterance_samples;
            }

            bool max_length = speech_started && speech_span_samples >= max_speech_samples;
            bool utterance_complete = speech_started &&
                                      (end_silence || max_length);

            if (utterance_complete && !speech_confirmed) {
                ESP_LOGW(TAG,
                         "VAD transient rejected active_ms=%u required_ms=%u; still listening",
                         (unsigned)samples_to_ms(active_speech_samples,
                                                 s_voice.sample_rate_hz),
                         (unsigned)VOICE_SERVICE_VAD_CONFIRM_MS);
                speech_started = false;
                speech_frames = 0;
                silence_frames = 0;
                speech_start_sample = 0;
                last_speech_sample = 0;
                realtime_streamed_samples = 0;
                utterance_complete = false;
            }

            bool no_speech_timeout = !speech_started &&
                                     frames_since_wake >= wait_frames;

            if (no_speech_timeout) {
                if (realtime_started) {
                    ESP_LOGI(TAG,
                             "No speech detected in %ums; closing realtime conversation",
                             (unsigned)VOICE_SERVICE_WAIT_SPEECH_MS);
                    err = voice_audio_capture_stop();
                    if (err != ESP_OK) {
                        if (realtime_sink.abort) realtime_sink.abort(realtime_ctx);
                        realtime_started = false;
                        status_set_error(err);
                        goto exit_task;
                    }
                    voice_audio_info_t audio_info = {0};
                    voice_audio_get_info(&audio_info);
                    if (audio_info.initialized && audio_info.state == VOICE_AUDIO_STATE_IDLE) {
                        esp_err_t deinit_err = voice_audio_deinit();
                        if (deinit_err != ESP_OK) {
                            ESP_LOGW(TAG, "audio deinit before realtime close failed: %s",
                                     esp_err_to_name(deinit_err));
                        }
                    }
                    release_sr_resources();
                    err = realtime_sink.close ?
                          realtime_sink.close(realtime_ctx) : ESP_ERR_NOT_SUPPORTED;
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Realtime close after no-speech timeout failed: %s",
                                 esp_err_to_name(err));
                        if (realtime_sink.abort) realtime_sink.abort(realtime_ctx);
                        status_set_error(err);
                    } else {
                        ESP_LOGI(TAG, "Realtime conversation closing normally after silence");
                    }
                    realtime_started = false;
                    goto exit_task;
                }
                ESP_LOGW(TAG, "VAD timeout: no command after wake; back to LISTENING");
                recording = false;
                speech_started = false;
                speech_confirmed = false;
                frames_since_wake = 0;
                speech_frames = 0;
                silence_frames = 0;
                utterance_samples = 0;
                realtime_streamed_samples = 0;
                rearm_frames = rearm_frames_needed;
                status_set_state(VOICE_SERVICE_STATE_LISTENING);
                ESP_LOGI(TAG, "WakeNet re-arm cooldown=%ums", (unsigned)VOICE_SERVICE_WAKE_REARM_MS);
            } else if (utterance_complete) {
                size_t pre_roll = ms_to_samples(VOICE_SERVICE_PRE_ROLL_MS, s_voice.sample_rate_hz);
                size_t post_roll = ms_to_samples(VOICE_SERVICE_POST_ROLL_MS, s_voice.sample_rate_hz);
                size_t begin = speech_start_sample > pre_roll ?
                               speech_start_sample - pre_roll : 0;
                size_t end = last_speech_sample + post_roll;
                if (end > utterance_samples) {
                    end = utterance_samples;
                }

                uint32_t utterance_ms = samples_to_ms(end - begin, s_voice.sample_rate_hz);
                portENTER_CRITICAL(&s_mux);
                s_voice.utterance_count++;
                s_voice.last_utterance_ms = utterance_ms;
                portEXIT_CRITICAL(&s_mux);

                if (realtime_started) {
                    ESP_LOGI(TAG,
                             "MIC-ASR capture complete reason=%s frames=%u samples=%u captured_ms=%u speech_ms=%u cloud_ms=%u; stopping I2S then draining realtime uplink",
                             max_length ? "max" : "silence",
                             (unsigned)frames_since_wake,
                             (unsigned)utterance_samples,
                             (unsigned)samples_to_ms(utterance_samples, s_voice.sample_rate_hz),
                             (unsigned)utterance_ms,
                             (unsigned)samples_to_ms(realtime_streamed_samples,
                                                     s_voice.sample_rate_hz));
                } else {
                    ESP_LOGI(TAG,
                             "VAD speech end reason=%s captured=%ums echo=%ums",
                             max_length ? "max" : "silence",
                             (unsigned)samples_to_ms(utterance_samples, s_voice.sample_rate_hz),
                             (unsigned)utterance_ms);
                }

                recording = false;

                voice_service_utterance_sink_fn sink = NULL;
                portENTER_CRITICAL(&s_mux);
                sink = s_utterance_sink;
                portEXIT_CRITICAL(&s_mux);

                if (realtime_started) {
                    err = voice_audio_capture_stop();
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "capture stop before realtime commit failed: %s", esp_err_to_name(err));
                        if (realtime_sink.abort) realtime_sink.abort(realtime_ctx);
                        realtime_started = false;
                        status_set_error(err);
                        goto exit_task;
                    }
                    voice_audio_info_t audio_info = {0};
                    voice_audio_get_info(&audio_info);
                    if (audio_info.initialized && audio_info.state == VOICE_AUDIO_STATE_IDLE) {
                        esp_err_t deinit_err = voice_audio_deinit();
                        if (deinit_err != ESP_OK) {
                            ESP_LOGW(TAG, "audio deinit before realtime commit failed: %s",
                                     esp_err_to_name(deinit_err));
                        }
                    }
                    release_sr_resources();
                    err = realtime_sink.end(realtime_ctx);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Realtime voice agent commit failed: %s", esp_err_to_name(err));
                        if (realtime_sink.abort) realtime_sink.abort(realtime_ctx);
                        status_set_error(err);
                    } else {
                        ESP_LOGI(TAG,
                                 "MIC-ASR microphone committed dynamic_vad speech_ms=%u",
                                 (unsigned)utterance_ms);
                    }
                    realtime_started = false;
                    goto exit_task;
                }

                if (sink) {
                    err = handoff_utterance(begin, end);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "dialog handoff failed: %s", esp_err_to_name(err));
                        status_set_error(err);
                    }
                    goto exit_task;
                }

                err = echo_utterance(begin, end);
                if (err != ESP_OK) {
                    if (!stop_requested()) {
                        ESP_LOGE(TAG, "echo cycle failed: %s", esp_err_to_name(err));
                        status_set_error(err);
                    }
                    break;
                }

                speech_started = false;
                speech_confirmed = false;
                utterance_samples = 0;
                realtime_streamed_samples = 0;
                rearm_frames = rearm_frames_needed;
                ESP_LOGI(TAG, "WakeNet re-arm cooldown=%ums", (unsigned)VOICE_SERVICE_WAKE_REARM_MS);
                ESP_LOGI(TAG, "LISTENING: say '%s'", s_voice.wake_word[0] ? s_voice.wake_word : "Hi,ESP");
            }
        }

        if ((frames_total % 320U) == 0U) {
            UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
            uint32_t bytes = (uint32_t)hw * sizeof(StackType_t);
            portENTER_CRITICAL(&s_mux);
            s_voice.task_stack_high_water_bytes = bytes;
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG,
                     "health state=%s noise=%u rms=%u peak=%u stack_free=%uB",
                     voice_service_state_name(s_voice.state),
                     (unsigned)s_voice.noise_rms,
                     (unsigned)rms,
                     (unsigned)peak,
                     (unsigned)bytes);
        }
    }

    if (voice_audio_capture_stop() != ESP_OK) {
        /* It may already be idle if playback/capture transition failed. */
    }

exit_task:
    if (realtime_started) {
        ESP_LOGW(TAG, "aborting active realtime session during Voice Service cleanup");
        if (realtime_sink.abort) {
            realtime_sink.abort(realtime_ctx);
        }
        realtime_started = false;
    }
    release_sr_resources();
    /*
     * V2.4.7: an early realtime transport failure can jump here while I2S is
     * still in CAPTURE (for example, session.create rejected by the server).
     * The old cleanup only deinitialized an already-IDLE device, leaving the
     * capture channels enabled. The re-armed Voice Service then failed with
     * "Audio is busy" and wake stopped working after the first attempt.
     *
     * Always bring voice_audio back to IDLE first, then release its DMA.
     */
    voice_audio_info_t exit_audio_info = {0};
    voice_audio_get_info(&exit_audio_info);
    if (exit_audio_info.initialized) {
        if (exit_audio_info.state == VOICE_AUDIO_STATE_CAPTURE) {
            esp_err_t stop_err = voice_audio_capture_stop();
            if (stop_err != ESP_OK) {
                ESP_LOGW(TAG, "exit cleanup capture stop failed: %s",
                         esp_err_to_name(stop_err));
            }
        } else if (exit_audio_info.state == VOICE_AUDIO_STATE_PLAYBACK) {
            esp_err_t stop_err = voice_audio_playback_stop();
            if (stop_err != ESP_OK) {
                ESP_LOGW(TAG, "exit cleanup playback stop failed: %s",
                         esp_err_to_name(stop_err));
            }
        }
        voice_audio_get_info(&exit_audio_info);
        if (exit_audio_info.state == VOICE_AUDIO_STATE_IDLE) {
            esp_err_t deinit_err = voice_audio_deinit();
            if (deinit_err != ESP_OK) {
                ESP_LOGW(TAG, "exit cleanup audio deinit failed: %s",
                         esp_err_to_name(deinit_err));
            }
        }
    }
    log_heap("after Voice Service cleanup");

    portENTER_CRITICAL(&s_mux);
    s_voice.running = false;
    s_voice.stop_requested = false;
    s_voice.followup_mode = false;
    s_voice.task = NULL;
    if (s_voice.state != VOICE_SERVICE_STATE_ERROR) {
        s_voice.state = VOICE_SERVICE_STATE_STOPPED;
        s_voice.last_error = ESP_OK;
    }
    portEXIT_CRITICAL(&s_mux);

    vTaskDelete(NULL);
}

const char *voice_service_state_name(voice_service_state_t state)
{
    switch (state) {
    case VOICE_SERVICE_STATE_STOPPED:   return "stopped";
    case VOICE_SERVICE_STATE_STARTING:  return "starting";
    case VOICE_SERVICE_STATE_LISTENING: return "listening";
    case VOICE_SERVICE_STATE_RECORDING: return "recording";
    case VOICE_SERVICE_STATE_PLAYBACK:  return "playback";
    case VOICE_SERVICE_STATE_ERROR:     return "error";
    default:                            return "unknown";
    }
}

static esp_err_t voice_service_start_internal(bool followup_mode)
{
    portENTER_CRITICAL(&s_mux);
    if (s_voice.running || s_voice.state == VOICE_SERVICE_STATE_STARTING) {
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }
    if (followup_mode &&
        (!s_realtime_sink.pcm || !s_realtime_sink.end || !s_realtime_sink.close)) {
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }
    s_voice.state = VOICE_SERVICE_STATE_STARTING;
    s_voice.last_error = ESP_OK;
    s_voice.running = true;
    s_voice.stop_requested = false;
    s_voice.followup_mode = followup_mode;
    if (!followup_mode) {
        s_voice.noise_rms = 0;
    }
    s_voice.last_rms = 0;
    s_voice.last_peak = 0;
    s_voice.last_utterance_ms = 0;
    s_voice.task_stack_high_water_bytes = 0;
    s_voice.model_name[0] = '\0';
    s_voice.wake_word[0] = '\0';
    portEXIT_CRITICAL(&s_mux);

    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(
        voice_service_task,
        "voice_sr",
        VOICE_SERVICE_TASK_STACK_BYTES,
        NULL,
        VOICE_SERVICE_TASK_PRIORITY,
        &task,
        VOICE_SERVICE_TASK_CORE);
    if (created != pdPASS) {
        ESP_LOGE(TAG,
                 "Failed to create voice task stack=%u; internal largest=%u",
                 (unsigned)VOICE_SERVICE_TASK_STACK_BYTES,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        release_sr_resources();
        portENTER_CRITICAL(&s_mux);
        s_voice.running = false;
        s_voice.followup_mode = false;
        s_voice.task = NULL;
        s_voice.state = VOICE_SERVICE_STATE_ERROR;
        s_voice.last_error = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_mux);
    if (s_voice.running) {
        s_voice.task = task;
    }
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "Voice Service %s worker created stack=%u core=%d",
             followup_mode ? "follow-up" : "WakeNet",
             (unsigned)VOICE_SERVICE_TASK_STACK_BYTES,
             VOICE_SERVICE_TASK_CORE);
    return ESP_OK;
}

esp_err_t voice_service_start(void)
{
    return voice_service_start_internal(false);
}

esp_err_t voice_service_start_followup(void)
{
    return voice_service_start_internal(true);
}

esp_err_t voice_service_set_utterance_sink(voice_service_utterance_sink_fn sink,
                                          void *user_ctx)
{
    portENTER_CRITICAL(&s_mux);
    s_utterance_sink = sink;
    s_utterance_sink_ctx = user_ctx;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t voice_service_set_realtime_sink(const voice_service_realtime_sink_t *sink,
                                          void *user_ctx)
{
    portENTER_CRITICAL(&s_mux);
    if (sink) {
        s_realtime_sink = *sink;
        s_realtime_sink_ctx = user_ctx;
    } else {
        memset(&s_realtime_sink, 0, sizeof(s_realtime_sink));
        s_realtime_sink_ctx = NULL;
    }
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t voice_service_stop(void)
{
    portENTER_CRITICAL(&s_mux);
    bool running = s_voice.running;
    if (running) {
        s_voice.stop_requested = true;
    }
    portEXIT_CRITICAL(&s_mux);

    if (!running) {
        return ESP_OK;
    }

    for (int i = 0; i < 40; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
        portENTER_CRITICAL(&s_mux);
        running = s_voice.running;
        portEXIT_CRITICAL(&s_mux);
        if (!running) {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

bool voice_service_is_running(void)
{
    bool running;
    portENTER_CRITICAL(&s_mux);
    running = s_voice.running;
    portEXIT_CRITICAL(&s_mux);
    return running;
}

void voice_service_get_status(voice_service_status_t *status)
{
    if (!status) {
        return;
    }
    memset(status, 0, sizeof(*status));

    portENTER_CRITICAL(&s_mux);
    status->running = s_voice.running;
    status->state = s_voice.state;
    status->last_error = s_voice.last_error;
    status->wake_count = s_voice.wake_count;
    status->utterance_count = s_voice.utterance_count;
    status->frame_samples = (uint32_t)s_voice.frame_samples;
    status->sample_rate_hz = s_voice.sample_rate_hz;
    status->noise_rms = s_voice.noise_rms;
    status->last_rms = s_voice.last_rms;
    status->last_peak = s_voice.last_peak;
    status->last_utterance_ms = s_voice.last_utterance_ms;
    status->task_stack_high_water_bytes = s_voice.task_stack_high_water_bytes;
    strlcpy(status->model_name, s_voice.model_name, sizeof(status->model_name));
    strlcpy(status->wake_word, s_voice.wake_word, sizeof(status->wake_word));
    portEXIT_CRITICAL(&s_mux);

    status->internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    status->internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    status->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}
