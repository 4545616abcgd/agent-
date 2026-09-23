/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTC_AUDIO_SAMPLE_RATE_HZ 16000U
#define RTC_AUDIO_FRAME_MS       10U
#define RTC_AUDIO_FRAME_SAMPLES  (RTC_AUDIO_SAMPLE_RATE_HZ * RTC_AUDIO_FRAME_MS / 1000U)

esp_err_t rtc_audio_start(void);
esp_err_t rtc_audio_stop(void);

/* Recorder-SR input callback. Output is interleaved 16-bit "MR": mic, ref. */
int rtc_audio_read_aec(void *buffer, int size, void *user_ctx, TickType_t ticks);

/* Queue decoded mono 16-bit PCM for real-time playback. */
esp_err_t rtc_audio_play_pcm(const int16_t *samples,
                             size_t sample_count,
                             TickType_t ticks);

void rtc_audio_flush_playback(void);

#ifdef __cplusplus
}
#endif
