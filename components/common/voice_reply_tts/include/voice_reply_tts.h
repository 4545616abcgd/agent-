/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Preload and retain the local ESP-TTS voice data in PSRAM.
 * Safe to call once at boot; later replies avoid repeated SD reads.
 */
esp_err_t voice_reply_tts_preload(void);

/**
 * Queue one final ESP-Claw reply for local ESP-TTS playback.
 *
 * The call is non-blocking. Text is copied to PSRAM and spoken by a transient
 * worker task, so the Event Router / Web IM callback is not blocked by TTS.
 *
 * Queue policy: one active utterance + one latest pending reply. If another
 * reply arrives while speaking, the pending reply is replaced by the newest.
 * Realtime voice conversations own I2S: queued text waits until the complete
 * multi-turn session is closed, then Voice Service is paused for playback.
 */
esp_err_t voice_reply_tts_enqueue(const char *utf8_text);

/**
 * Queue a reply and force Voice Service to restart after playback.
 * This is used by the voice-dialog path after WakeNet/I2S were released.
 */
esp_err_t voice_reply_tts_enqueue_and_restart(const char *utf8_text);

/** True while a reply worker is active or a reply is pending. */
bool voice_reply_tts_is_busy(void);

#ifdef __cplusplus
}
#endif
