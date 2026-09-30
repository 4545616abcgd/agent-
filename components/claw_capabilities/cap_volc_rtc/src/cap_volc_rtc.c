/* SPDX-License-Identifier: Apache-2.0 */

#include "cap_volc_rtc.h"

#include <stdio.h>

#include "claw_cap.h"
#include "rtc_agent.h"

static esp_err_t write_status(char *output, size_t output_size)
{
    rtc_agent_status_t status = {0};
    esp_err_t err = rtc_agent_get_status(&status);
    if (err != ESP_OK) {
        return err;
    }
    int written = snprintf(output, output_size,
                           "{\"ready\":%s,\"configured\":%s,\"engine_ready\":%s,"
                           "\"session_started\":%s,\"room_connected\":%s,"
                           "\"remote_agent_joined\":%s}",
                           status.ready ? "true" : "false",
                           status.configured ? "true" : "false",
                           status.engine_ready ? "true" : "false",
                           status.session_started ? "true" : "false",
                           status.room_connected ? "true" : "false",
                           status.remote_agent_joined ? "true" : "false");
    return written >= 0 && (size_t)written < output_size
               ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t status_execute(const char *input_json,
                                const claw_cap_call_context_t *ctx,
                                char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return write_status(output, output_size);
}

static esp_err_t start_execute(const char *input_json,
                               const claw_cap_call_context_t *ctx,
                               char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = rtc_agent_request_start();
    if (err != ESP_OK) {
        snprintf(output, output_size, "RTC start request failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    return write_status(output, output_size);
}

static esp_err_t stop_execute(const char *input_json,
                              const claw_cap_call_context_t *ctx,
                              char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = rtc_agent_request_stop();
    if (err != ESP_OK) {
        snprintf(output, output_size, "RTC stop request failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    return write_status(output, output_size);
}

static const claw_cap_descriptor_t s_descriptors[] = {
    {
        .id = "rtc_voice_get_status",
        .name = "rtc_voice_get_status",
        .family = "voice",
        .description = "Read the hardware-agent RTC voice service and room state. Does not start a conversation.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
        .execute = status_execute,
    },
    {
        .id = "rtc_voice_start",
        .name = "rtc_voice_start",
        .family = "voice",
        .description = "Start the Volc hardware-agent voice conversation only when the user explicitly requests live voice chat.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
        .execute = start_execute,
    },
    {
        .id = "rtc_voice_stop",
        .name = "rtc_voice_stop",
        .family = "voice",
        .description = "End the current Volc hardware-agent voice conversation when the user requests it.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
        .execute = stop_execute,
    },
};

static const claw_cap_group_t s_group = {
    .group_id = "cap_volc_rtc",
    .descriptors = s_descriptors,
    .descriptor_count = sizeof(s_descriptors) / sizeof(s_descriptors[0]),
};

esp_err_t cap_volc_rtc_register_group(void)
{
    if (claw_cap_group_exists(s_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_group);
}
