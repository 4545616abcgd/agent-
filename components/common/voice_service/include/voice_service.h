/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_SERVICE_STATE_STOPPED = 0,
    VOICE_SERVICE_STATE_STARTING,
    VOICE_SERVICE_STATE_LISTENING,
    VOICE_SERVICE_STATE_RECORDING,
    VOICE_SERVICE_STATE_PLAYBACK,
    VOICE_SERVICE_STATE_ERROR,
} voice_service_state_t;

typedef esp_err_t (*voice_service_utterance_sink_fn)(int16_t *pcm_owned,
                                                     size_t sample_count,
                                                     uint32_t sample_rate_hz,
                                                     void *user_ctx);
typedef esp_err_t (*voice_service_realtime_begin_fn)(uint32_t sample_rate_hz, void *user_ctx);
typedef esp_err_t (*voice_service_realtime_pcm_fn)(const int16_t *samples,
                                                    size_t sample_count,
                                                    uint32_t sample_rate_hz,
                                                    void *user_ctx);
typedef esp_err_t (*voice_service_realtime_end_fn)(void *user_ctx);
typedef esp_err_t (*voice_service_realtime_close_fn)(void *user_ctx);
typedef void (*voice_service_realtime_abort_fn)(void *user_ctx);

typedef struct {
    voice_service_realtime_begin_fn begin;
    voice_service_realtime_pcm_fn pcm;
    voice_service_realtime_end_fn end;
    voice_service_realtime_close_fn close;
    voice_service_realtime_abort_fn abort;
} voice_service_realtime_sink_t;

typedef struct {
    bool running;
    voice_service_state_t state;
    esp_err_t last_error;
    uint32_t wake_count;
    uint32_t utterance_count;
    uint32_t frame_samples;
    uint32_t sample_rate_hz;
    uint32_t noise_rms;
    uint32_t last_rms;
    uint32_t last_peak;
    uint32_t last_utterance_ms;
    uint32_t task_stack_high_water_bytes;
    size_t internal_free;
    size_t internal_largest;
    size_t psram_free;
    char model_name[64];
    char wake_word[64];
} voice_service_status_t;

/**
 * Start the low-RAM Voice V1 pipeline on demand.
 *
 * Product pipeline (V2.4):
 *   WakeNet -> realtime PCM sink -> Voice Agent / cloud realtime backend.
 * Local VAD remains a turn-boundary helper; a missed local speech start no longer
 * discards an already-streamed realtime turn.
 *
 * This API itself remains explicit/start-on-request. Product builds may call
 * voice_service_start() automatically during boot. Startup must remain fail-soft:
 * voice hardware/model/RAM failure must not abort the product runtime.
 */
esp_err_t voice_service_start(void);

/**
 * Capture one follow-up turn without loading WakeNet or replaying the wake beep.
 * The realtime sink must already have resumed an existing cloud session.
 */
esp_err_t voice_service_start_followup(void);

/**
 * Register a completed-utterance sink.
 *
 * With a sink registered, Voice Service transfers ownership of a PSRAM PCM16
 * mono buffer to the sink after VAD completes one command. The sink must
 * eventually free() the buffer. Returning ESP_OK means ownership was accepted.
 * With no sink registered, the legacy local-echo validation path is retained.
 */
esp_err_t voice_service_set_utterance_sink(voice_service_utterance_sink_fn sink,
                                          void *user_ctx);
/** Register realtime PCM hooks used after WakeNet triggers. */
esp_err_t voice_service_set_realtime_sink(const voice_service_realtime_sink_t *sink,
                                          void *user_ctx);

/** Stop listening, release WakeNet/model buffers, and return audio to idle. */
esp_err_t voice_service_stop(void);

/** True while the worker is alive (LISTENING/RECORDING/PLAYBACK). */
bool voice_service_is_running(void);

/** Snapshot service state and memory telemetry. */
void voice_service_get_status(voice_service_status_t *status);

const char *voice_service_state_name(voice_service_state_t state);

#ifdef __cplusplus
}
#endif
