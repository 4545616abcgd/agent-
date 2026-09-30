/* SPDX-License-Identifier: Apache-2.0 */

#include "rtc_message.h"

#include <stdio.h>
#include <string.h>

#include "claw_cap.h"
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "rtc_tools";

static bool is_allowed_tool(const char *name)
{
    static const char *const allowed[] = {
        "weather_get_current",
        "weather_get_hourly",
        "weather_get_daily",
        "weather_get_alerts",
        "weather_get_status",
        "get_system_info",
        "get_current_time",
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        if (strcmp(name, allowed[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool valid_arguments(const char *name, const cJSON *root)
{
    if (!cJSON_IsObject(root)) {
        return false;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (!item->string) {
            return false;
        }
        if ((strcmp(name, "weather_get_hourly") == 0 && strcmp(item->string, "hours") == 0) ||
            (strcmp(name, "weather_get_daily") == 0 && strcmp(item->string, "days") == 0)) {
            int max = strcmp(name, "weather_get_hourly") == 0 ? 24 : 7;
            if (!cJSON_IsNumber(item) || item->valuedouble < 1 ||
                item->valuedouble > max || item->valuedouble != item->valueint) {
                return false;
            }
        } else if (strcmp(name, "weather_get_alerts") == 0 &&
                   strcmp(item->string, "include_details") == 0) {
            if (!cJSON_IsBool(item)) {
                return false;
            }
        } else if (strcmp(name, "get_system_info") == 0 &&
                   strcmp(item->string, "sections") == 0) {
            if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) == 0 ||
                cJSON_GetArraySize(item) > 7) {
                return false;
            }
            const cJSON *section = NULL;
            cJSON_ArrayForEach(section, item) {
                const char *value = cJSON_GetStringValue(section);
                if (!value || (strcmp(value, "chip") && strcmp(value, "uptime") &&
                    strcmp(value, "version") && strcmp(value, "memory") &&
                    strcmp(value, "cpu") && strcmp(value, "wifi") && strcmp(value, "ip"))) {
                    return false;
                }
                for (const cJSON *other = section->next; other; other = other->next) {
                    const char *other_value = cJSON_GetStringValue(other);
                    if (other_value && strcmp(value, other_value) == 0) {
                        return false;
                    }
                }
            }
        } else {
            /* Weather tools only query the device's configured location. */
            return false;
        }
        for (const cJSON *other = item->next; other; other = other->next) {
            if (other->string && strcmp(item->string, other->string) == 0) {
                return false;
            }
        }
    }
    return true;
}

esp_err_t rtc_message_dispatch_tool(const char *call_id,
                                    const char *name,
                                    const char *arguments_json,
                                    char *output_json,
                                    size_t output_capacity)
{
    if (!call_id || !name || !arguments_json || !output_json ||
        output_capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output_json[0] = '\0';
    if (!is_allowed_tool(name)) {
        snprintf(output_json, output_capacity,
                 "{\"ok\":false,\"error\":\"tool is not allowed on the RTC voice bridge\"}");
        return ESP_ERR_NOT_SUPPORTED;
    }

    cJSON *arguments = cJSON_ParseWithOpts(arguments_json, NULL, true);
    bool valid = valid_arguments(name, arguments);
    cJSON_Delete(arguments);
    if (!valid) {
        snprintf(output_json, output_capacity,
                 "{\"ok\":false,\"error\":\"invalid tool arguments; use the declared schema and configured device location\"}");
        return ESP_ERR_INVALID_ARG;
    }

    claw_cap_call_context_t context = {
        .session_id = "volc-rtc",
        .channel = "voice",
        .chat_id = "volc-rtc",
        .source_cap = "volc_rtc_voice",
        .correlation_id = call_id,
        .caller = CLAW_CAP_CALLER_AGENT,
    };
    return claw_cap_call(name, arguments_json, &context,
                         output_json, output_capacity);
}

void rtc_message_probe_tools(char *output, size_t capacity)
{
    if (!output || capacity == 0) {
        return;
    }
    static const char *const probes[] = {
        "weather_get_status", "weather_get_current", "get_system_info",
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        const char *arguments = strcmp(probes[i], "get_system_info") == 0
                                    ? "{\"sections\":[\"uptime\",\"memory\"]}" : "{}";
        esp_err_t err = rtc_message_dispatch_tool(
            "local-probe", probes[i], arguments, output, capacity);
        ESP_LOGI(TAG, "local probe name=%s dispatch=%s result=%.1200s",
                 probes[i], esp_err_to_name(err), output);
    }
    ESP_LOGI(TAG, "local probes only; cloud tool declarations and Bot consumption are not verified");
}
