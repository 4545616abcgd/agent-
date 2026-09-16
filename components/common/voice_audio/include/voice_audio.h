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
    VOICE_AUDIO_STATE_UNINITIALIZED = 0,
    VOICE_AUDIO_STATE_IDLE,
    VOICE_AUDIO_STATE_CAPTURE,
    VOICE_AUDIO_STATE_PLAYBACK,
} voice_audio_state_t;

typedef struct {
    uint32_t sample_rate_hz;
    uint8_t pcm_bits;
    uint8_t channels;
    int bclk_gpio;
    int ws_gpio;
    int mic_data_gpio;
    int spk_data_gpio;
    voice_audio_state_t state;
    bool initialized;
} voice_audio_info_t;

typedef struct {
    uint32_t samples;
    uint32_t pcm_bytes;
    int16_t peak;
    uint16_t rms;
} voice_audio_stats_t;

/**
 * Initialize the shared I2S STD RX/TX pair.
 *
 * The driver is initialized lazily; no task is created and no channel is
 * enabled until capture/playback starts.
 */
esp_err_t voice_audio_init(void);

/** Deinitialize the Audio HAL. Only valid while idle. */
esp_err_t voice_audio_deinit(void);

/** Query compile-time pin/format settings and current state. */
void voice_audio_get_info(voice_audio_info_t *info);

/**
 * Start/stop microphone capture.
 *
 * PCM exposed by capture_read is signed 16-bit, mono, at sample_rate_hz.
 * State acquisition is atomic. Concurrent capture/playback transitions, I/O,
 * or deinitialization return ESP_ERR_INVALID_STATE instead of sharing I2S.
 */
esp_err_t voice_audio_capture_start(void);
esp_err_t voice_audio_capture_read(int16_t *samples,
                                   size_t capacity_samples,
                                   size_t *out_samples,
                                   uint32_t timeout_ms);
esp_err_t voice_audio_capture_stop(void);

/**
 * Start/stop speaker playback.
 *
 * Input PCM is signed 16-bit mono. The HAL expands it to a 32-bit stereo I2S
 * frame so either MAX98357A channel selection receives the same audio.
 * State acquisition is atomic and only one staging-buffer I/O call may run.
 */
esp_err_t voice_audio_playback_start(void);
esp_err_t voice_audio_playback_write(const int16_t *samples,
                                     size_t sample_count,
                                     uint32_t timeout_ms);
esp_err_t voice_audio_playback_stop(void);

/** Record a WAV file using the streaming capture API. */
esp_err_t voice_audio_record_wav(const char *path,
                                 uint32_t duration_ms,
                                 voice_audio_stats_t *out_stats);

/** Play a PCM WAV file. Voice V1 accepts 16 kHz / mono / 16-bit PCM. */
esp_err_t voice_audio_play_wav(const char *path);

#ifdef __cplusplus
}
#endif
