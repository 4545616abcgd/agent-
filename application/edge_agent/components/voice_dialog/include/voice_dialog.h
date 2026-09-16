/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_DIALOG_ASR_KEY_MAX    256
#define VOICE_DIALOG_ASR_URL_MAX    320
#define VOICE_DIALOG_ASR_MODEL_MAX   96

typedef struct {
    char api_key[VOICE_DIALOG_ASR_KEY_MAX];
    char base_url[VOICE_DIALOG_ASR_URL_MAX];
    char model[VOICE_DIALOG_ASR_MODEL_MAX];
} voice_dialog_asr_config_t;

/** Initialize the V2.4 realtime-only Voice Agent adapter. Safe to call once during boot. */
esp_err_t voice_dialog_init(void);

/** Load current ASR config from the shared app settings namespace. */
esp_err_t voice_dialog_get_asr_config(voice_dialog_asr_config_t *out_config);

/** Save ASR config to the shared app settings namespace. */
esp_err_t voice_dialog_set_asr_config(const voice_dialog_asr_config_t *config);

/** True while the realtime Voice Agent turn is active. */
bool voice_dialog_is_busy(void);

#ifdef __cplusplus
}
#endif
