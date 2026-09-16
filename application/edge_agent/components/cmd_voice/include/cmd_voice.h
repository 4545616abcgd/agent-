/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the `voice` console command.
 *
 * storage_base_path is copied internally and must point to the active writable
 * storage root (normally /sdcard, otherwise /fatfs).
 */
esp_err_t register_voice_command(const char *storage_base_path);

#ifdef __cplusplus
}
#endif
