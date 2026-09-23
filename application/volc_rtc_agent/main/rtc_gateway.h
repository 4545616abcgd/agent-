/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char session_id[48];
    char app_id[80];
    char room_id[96];
    char user_id[128];
    char token[1536];
    int64_t expires_at;
} rtc_gateway_session_t;

bool rtc_gateway_is_configured(void);
esp_err_t rtc_gateway_start_session(rtc_gateway_session_t *session);
esp_err_t rtc_gateway_stop_session(const rtc_gateway_session_t *session);

#ifdef __cplusplus
}
#endif
