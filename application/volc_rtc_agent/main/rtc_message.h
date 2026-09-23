/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rtc_message_start(void);

void rtc_message_process(const void *message,
                         size_t size,
                         bool binary);

/*
 * edge_agent will provide a strong implementation when the RTC transport is
 * merged back into the full product. The standalone bring-up app keeps the
 * protocol parser active but returns ESP_ERR_NOT_SUPPORTED for capabilities.
 */
esp_err_t rtc_message_dispatch_tool(const char *name,
                                    const char *arguments_json,
                                    char *output_json,
                                    size_t output_capacity);

#ifdef __cplusplus
}
#endif
