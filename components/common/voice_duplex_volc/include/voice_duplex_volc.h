/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-Claw <-> Volcengine Doubao Realtime Duplex voice transport.
 * Product V1: PCM16/16 kHz uplink, pcm_s16le/24 kHz downlink.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*voice_duplex_volc_done_fn)(esp_err_t status, void *user_ctx);

typedef struct {
    /** Called after one response has finished playing while the cloud session stays open. */
    voice_duplex_volc_done_fn turn_done;
    /** Called exactly once after the complete cloud session has been released. */
    voice_duplex_volc_done_fn done;
    void *user_ctx;
} voice_duplex_volc_config_t;

/** Initialize the transport once at boot. This does not open a cloud session. */
esp_err_t voice_duplex_volc_init(const voice_duplex_volc_config_t *config);

/**
 * Start one realtime dialogue session. API key is loaded from settings:
 * db_key first, then the existing asr_key as product-v1 compatibility fallback.
 */
esp_err_t voice_duplex_volc_begin(uint32_t input_sample_rate_hz);

/** Wait until session.created has been received and the cloud session is ready. */
esp_err_t voice_duplex_volc_wait_ready(uint32_t timeout_ms);

/** Push signed PCM16 mono samples captured by Voice Service. Non-blocking. */
esp_err_t voice_duplex_volc_push_pcm(const int16_t *samples, size_t sample_count);

/**
 * Finish local microphone input. Buffered PCM is drained, then exactly one
 * input_audio_buffer.commit event is sent for this turn.
 */
esp_err_t voice_duplex_volc_commit(void);

/** Open microphone input for the next turn on the existing cloud session. */
esp_err_t voice_duplex_volc_resume_turn(void);

/** Gracefully close the current cloud session without treating silence as an error. */
esp_err_t voice_duplex_volc_close(void);

/** Abort the current realtime session. Safe to call repeatedly. */
void voice_duplex_volc_abort(void);

/** True while a cloud session/response is active. */
bool voice_duplex_volc_is_busy(void);

#ifdef __cplusplus
}
#endif
