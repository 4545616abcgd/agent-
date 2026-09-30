/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rtc_agent_start(void);
esp_err_t rtc_agent_send_tool_message(const void *data, size_t size);

typedef struct {
    bool ready;
    bool configured;
    bool engine_ready;
    bool session_started;
    bool room_connected;
    bool remote_agent_joined;
} rtc_agent_status_t;

esp_err_t rtc_agent_get_status(rtc_agent_status_t *status);
esp_err_t rtc_agent_request_start(void);
esp_err_t rtc_agent_request_stop(void);

#ifdef __cplusplus
}
#endif
