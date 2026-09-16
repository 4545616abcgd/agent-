/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t voice_data_bytes;
    uint64_t pcm_samples;
    uint32_t pcm_ms;
    size_t internal_free_after;
    size_t psram_free_after;
} voice_tts_result_t;

/**
 * Load and retain one ESP-TTS voice-data file in PSRAM.
 *
 * Calling this once during boot trades a few seconds of boot time for much
 * lower reply latency: later TTS calls reuse the same voice data instead of
 * reading it from SD for every answer.
 */
esp_err_t voice_tts_preload_file(const char *voice_data_path);

/**
 * Load an ESP-TTS voice-data .dat file from filesystem into PSRAM,
 * synthesize UTF-8 Chinese text, and stream PCM16/16k/mono to voice_audio.
 *
 * This is a runtime validation path. Keep Voice Service stopped while calling.
 */
esp_err_t voice_tts_speak_file(const char *voice_data_path,
                               const char *utf8_text,
                               unsigned int speed,
                               voice_tts_result_t *out_result);

#ifdef __cplusplus
}
#endif
