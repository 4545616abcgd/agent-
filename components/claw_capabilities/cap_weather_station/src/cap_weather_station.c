/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_weather_station.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "weather_service.h"

static const char *TAG = "cap_weather";

typedef struct {
    StaticSemaphore_t mutex_storage;
    SemaphoreHandle_t mutex;
    weather_snapshot_t *snapshot;
    bool snapshot_in_psram;
} weather_cap_runtime_t;

static weather_cap_runtime_t s_runtime;

static esp_err_t weather_runtime_init(void)
{
    if (s_runtime.mutex && s_runtime.snapshot) {
        return ESP_OK;
    }
    if (s_runtime.mutex || s_runtime.snapshot) {
        return ESP_ERR_INVALID_STATE;
    }

    SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&s_runtime.mutex_storage);
    if (!mutex) {
        return ESP_ERR_NO_MEM;
    }

    bool snapshot_in_psram = true;
    weather_snapshot_t *snapshot = heap_caps_calloc(
        1, sizeof(*snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot) {
        snapshot_in_psram = false;
        snapshot = heap_caps_calloc(1, sizeof(*snapshot), MALLOC_CAP_8BIT);
    }
    if (!snapshot) {
        return ESP_ERR_NO_MEM;
    }

    s_runtime.mutex = mutex;
    s_runtime.snapshot = snapshot;
    s_runtime.snapshot_in_psram = snapshot_in_psram;

    if (!snapshot_in_psram) {
        ESP_LOGW(TAG, "PSRAM scratch unavailable; using %u bytes of 8-bit heap",
                 (unsigned)sizeof(*snapshot));
    } else {
        ESP_LOGI(TAG, "weather snapshot scratch ready in PSRAM (%u bytes)",
                 (unsigned)sizeof(*snapshot));
    }
    return ESP_OK;
}

static esp_err_t weather_snapshot_acquire(weather_snapshot_t **out_snapshot)
{
    if (!out_snapshot) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_snapshot = NULL;

    if (!s_runtime.mutex || !s_runtime.snapshot) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_runtime.mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    memset(s_runtime.snapshot, 0, sizeof(*s_runtime.snapshot));
    *out_snapshot = s_runtime.snapshot;
    return ESP_OK;
}

static void weather_snapshot_release(weather_snapshot_t *snapshot)
{
    if (!snapshot || snapshot != s_runtime.snapshot || !s_runtime.mutex) {
        ESP_LOGE(TAG, "invalid weather snapshot release");
        return;
    }
    if (xSemaphoreGive(s_runtime.mutex) != pdTRUE) {
        ESP_LOGE(TAG, "failed to release weather snapshot scratch");
    }
}

static esp_err_t write_json(cJSON *root, char *output, size_t output_size)
{
    if (!root || !output || output_size == 0) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"json allocation failed\"}");
        return ESP_ERR_NO_MEM;
    }

    size_t len = strlen(text);
    if (len >= output_size) {
        free(text);
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"weather result exceeds tool output buffer\"}");
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(output, text, len + 1);
    free(text);
    return ESP_OK;
}

static cJSON *make_error(const char *message, esp_err_t code)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message ? message : "weather error");
    cJSON_AddStringToObject(root, "code", esp_err_to_name(code));
    return root;
}

static esp_err_t finish_error(cJSON *root,
                              esp_err_t code,
                              char *output,
                              size_t output_size)
{
    if (!root) {
        if (output && output_size > 0) {
            snprintf(output, output_size,
                     "{\"ok\":false,\"error\":\"error response allocation failed\","
                     "\"code\":\"%s\"}",
                     esp_err_to_name(code));
        }
        return code;
    }

    esp_err_t emit_err = write_json(root, output, output_size);
    return emit_err == ESP_OK ? code : emit_err;
}

static esp_err_t parse_limit(const char *input_json,
                             const char *field,
                             int default_value,
                             int min_value,
                             int max_value,
                             int *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = default_value;
    cJSON *input = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!input || !cJSON_IsObject(input)) {
        cJSON_Delete(input);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(input, field);
    if (item) {
        if (!cJSON_IsNumber(item)) {
            cJSON_Delete(input);
            return ESP_ERR_INVALID_ARG;
        }
        int value = item->valueint;
        if (value < min_value || value > max_value) {
            cJSON_Delete(input);
            return ESP_ERR_INVALID_ARG;
        }
        *out = value;
    }

    cJSON_Delete(input);
    return ESP_OK;
}

static void add_cache_meta(cJSON *root,
                           const weather_service_status_t *status,
                           const weather_snapshot_t *snap)
{
    time_t now = time(NULL);
    int64_t age_sec = -1;
    bool stale = true;

    if (status->last_success_at > 0 && now >= status->last_success_at) {
        age_sec = (int64_t)(now - status->last_success_at);
        uint32_t stale_after_sec = (status->refresh_interval_ms / 1000U) * 2U;
        if (stale_after_sec == 0) {
            stale_after_sec = 3600U;
        }
        stale = (uint64_t)age_sec > stale_after_sec;
    }

    cJSON_AddStringToObject(root, "provider",
                           snap->provider[0] ? snap->provider :
                           (status->provider[0] ? status->provider : "unknown"));
    cJSON_AddNumberToObject(root, "last_success_epoch", (double)status->last_success_at);
    cJSON_AddNumberToObject(root, "cache_age_sec", (double)age_sec);
    cJSON_AddBoolToObject(root, "stale", stale);
    if (snap->attribution[0]) {
        cJSON_AddStringToObject(root, "attribution", snap->attribution);
    }
}

static esp_err_t get_snapshot_or_error(weather_snapshot_t **out_snap,
                                       weather_service_status_t *status,
                                       uint32_t required_mask,
                                       char *output,
                                       size_t output_size)
{
    if (!out_snap || !status || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_snap = NULL;
    memset(status, 0, sizeof(*status));

    weather_snapshot_t *snap = NULL;
    esp_err_t err = weather_snapshot_acquire(&snap);
    if (err != ESP_OK) {
        return finish_error(make_error("weather snapshot scratch unavailable", err),
                            err, output, output_size);
    }

    weather_service_get_status(status);

    err = weather_service_get_snapshot(snap);
    if (err != ESP_OK) {
        weather_snapshot_release(snap);
        return finish_error(make_error("weather service cache unavailable", err),
                            err, output, output_size);
    }

    if ((snap->valid_mask & required_mask) != required_mask) {
        cJSON *root = make_error("requested weather data is not cached yet",
                                 ESP_ERR_INVALID_STATE);
        if (root) {
            cJSON_AddBoolToObject(root, "configured", status->provider_configured);
            cJSON_AddBoolToObject(root, "network_online", status->network_online);
            cJSON_AddNumberToObject(root, "last_success_epoch",
                                    (double)status->last_success_at);
        }
        weather_snapshot_release(snap);
        return finish_error(root, ESP_ERR_INVALID_STATE, output, output_size);
    }

    *out_snap = snap;
    return ESP_OK;
}

static esp_err_t write_snapshot_json(cJSON *root,
                                     weather_snapshot_t *snap,
                                     char *output,
                                     size_t output_size)
{
    esp_err_t err = write_json(root, output, output_size);
    weather_snapshot_release(snap);
    return err;
}

static void add_current_fields(cJSON *root, const weather_current_t *w)
{
    cJSON_AddStringToObject(root, "condition", w->condition_text);
    cJSON_AddStringToObject(root, "condition_code", w->condition_code);
    cJSON_AddNumberToObject(root, "temperature_c", w->temperature_c);
    cJSON_AddNumberToObject(root, "feels_like_c", w->feels_like_c);
    cJSON_AddNumberToObject(root, "humidity_pct", w->humidity_pct);
    cJSON_AddNumberToObject(root, "pressure_hpa", w->pressure_hpa);
    cJSON_AddNumberToObject(root, "precipitation_mm", w->precipitation_mm);
    cJSON_AddNumberToObject(root, "precipitation_intensity_mm_h",
                           w->precipitation_intensity_mm_h);
    cJSON_AddNumberToObject(root, "visibility_km", w->visibility_km);
    cJSON_AddNumberToObject(root, "dew_point_c", w->dew_point_c);
    cJSON_AddNumberToObject(root, "cloud_cover_pct", w->cloud_cover_pct);
    cJSON_AddNumberToObject(root, "uv_index", w->uv_index);
    cJSON_AddNumberToObject(root, "wind_speed_mps", w->wind_speed_mps);
    cJSON_AddNumberToObject(root, "wind_gust_mps", w->wind_gust_mps);
    cJSON_AddNumberToObject(root, "wind_direction_deg", w->wind_direction_deg);
    cJSON_AddNumberToObject(root, "wind_scale", w->wind_scale);
    cJSON_AddStringToObject(root, "wind_compass", w->wind_compass);
    cJSON_AddStringToObject(root, "precipitation_type", w->precipitation_type);
}

static esp_err_t cap_weather_get_current_execute(const char *input_json,
                                                 const claw_cap_call_context_t *ctx,
                                                 char *output,
                                                 size_t output_size)
{
    (void)input_json;
    (void)ctx;

    weather_snapshot_t *snap = NULL;
    weather_service_status_t status;
    esp_err_t err = get_snapshot_or_error(&snap, &status, WEATHER_SNAPSHOT_CURRENT,
                                          output, output_size);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        weather_snapshot_release(snap);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    add_cache_meta(root, &status, snap);
    cJSON_AddNumberToObject(root, "fetched_epoch", (double)snap->current.fetched_at);
    add_current_fields(root, &snap->current);
    return write_snapshot_json(root, snap, output, output_size);
}

static esp_err_t cap_weather_get_hourly_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size)
{
    (void)ctx;

    int hours = 6;
    if (parse_limit(input_json, "hours", 6, 1, (int)WEATHER_MAX_HOURLY, &hours) != ESP_OK) {
        return finish_error(make_error("hours must be an integer from 1 to 24",
                                        ESP_ERR_INVALID_ARG),
                            ESP_ERR_INVALID_ARG, output, output_size);
    }

    weather_snapshot_t *snap = NULL;
    weather_service_status_t status;
    esp_err_t err = get_snapshot_or_error(&snap, &status, WEATHER_SNAPSHOT_HOURLY,
                                          output, output_size);
    if (err != ESP_OK) {
        return err;
    }

    size_t count = snap->hourly_count;
    if (count > (size_t)hours) {
        count = (size_t)hours;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!root || !items) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        weather_snapshot_release(snap);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    add_cache_meta(root, &status, snap);
    cJSON_AddNumberToObject(root, "count", (double)count);
    if (!cJSON_AddItemToObject(root, "hours", items)) {
        cJSON_Delete(items);
        cJSON_Delete(root);
        weather_snapshot_release(snap);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < count; i++) {
        const weather_hourly_t *w = &snap->hourly[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            weather_snapshot_release(snap);
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(item, "time", w->forecast_time);
        cJSON_AddStringToObject(item, "condition", w->condition_text);
        cJSON_AddStringToObject(item, "condition_code", w->condition_code);
        cJSON_AddNumberToObject(item, "temperature_c", w->temperature_c);
        cJSON_AddNumberToObject(item, "feels_like_c", w->feels_like_c);
        cJSON_AddNumberToObject(item, "humidity_pct", w->humidity_pct);
        cJSON_AddNumberToObject(item, "pressure_hpa", w->pressure_hpa);
        cJSON_AddNumberToObject(item, "precipitation_mm", w->precipitation_mm);
        cJSON_AddNumberToObject(item, "precipitation_probability_pct",
                               w->precipitation_probability_pct);
        cJSON_AddNumberToObject(item, "wind_speed_mps", w->wind_speed_mps);
        cJSON_AddNumberToObject(item, "wind_gust_mps", w->wind_gust_mps);
        cJSON_AddNumberToObject(item, "wind_direction_deg", w->wind_direction_deg);
        cJSON_AddNumberToObject(item, "wind_scale", w->wind_scale);
        cJSON_AddStringToObject(item, "wind_compass", w->wind_compass);
        if (!cJSON_AddItemToArray(items, item)) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            weather_snapshot_release(snap);
            return ESP_ERR_NO_MEM;
        }
    }

    return write_snapshot_json(root, snap, output, output_size);
}

static esp_err_t cap_weather_get_daily_execute(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    (void)ctx;

    int days = 7;
    if (parse_limit(input_json, "days", 7, 1, (int)WEATHER_MAX_DAILY, &days) != ESP_OK) {
        return finish_error(make_error("days must be an integer from 1 to 7",
                                        ESP_ERR_INVALID_ARG),
                            ESP_ERR_INVALID_ARG, output, output_size);
    }

    weather_snapshot_t *snap = NULL;
    weather_service_status_t status;
    esp_err_t err = get_snapshot_or_error(&snap, &status, WEATHER_SNAPSHOT_DAILY,
                                          output, output_size);
    if (err != ESP_OK) {
        return err;
    }

    size_t count = snap->daily_count;
    if (count > (size_t)days) {
        count = (size_t)days;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!root || !items) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        weather_snapshot_release(snap);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    add_cache_meta(root, &status, snap);
    cJSON_AddNumberToObject(root, "count", (double)count);
    if (!cJSON_AddItemToObject(root, "days", items)) {
        cJSON_Delete(items);
        cJSON_Delete(root);
        weather_snapshot_release(snap);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < count; i++) {
        const weather_daily_t *w = &snap->daily[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            weather_snapshot_release(snap);
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(item, "start", w->forecast_start);
        cJSON_AddStringToObject(item, "end", w->forecast_end);
        cJSON_AddNumberToObject(item, "temperature_min_c", w->temperature_min_c);
        cJSON_AddNumberToObject(item, "temperature_max_c", w->temperature_max_c);
        cJSON_AddNumberToObject(item, "temperature_avg_c", w->temperature_avg_c);
        cJSON_AddNumberToObject(item, "uv_index_max", w->uv_index_max);
        cJSON_AddStringToObject(item, "sunrise", w->sunrise);
        cJSON_AddStringToObject(item, "sunset", w->sunset);
        cJSON_AddStringToObject(item, "moon_phase", w->moon_phase);

        cJSON *day = cJSON_AddObjectToObject(item, "daytime");
        if (!day) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            weather_snapshot_release(snap);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(day, "condition", w->daytime_text);
        cJSON_AddStringToObject(day, "condition_code", w->daytime_code);
        cJSON_AddNumberToObject(day, "humidity_pct", w->daytime_humidity_pct);
        cJSON_AddNumberToObject(day, "precipitation_mm", w->daytime_precip_mm);
        cJSON_AddNumberToObject(day, "precipitation_probability_pct",
                               w->daytime_precip_probability_pct);
        cJSON_AddNumberToObject(day, "wind_speed_mps", w->daytime_wind_speed_mps);
        cJSON_AddNumberToObject(day, "wind_scale", w->daytime_wind_scale);

        cJSON *night = cJSON_AddObjectToObject(item, "nighttime");
        if (!night) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            weather_snapshot_release(snap);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(night, "condition", w->nighttime_text);
        cJSON_AddStringToObject(night, "condition_code", w->nighttime_code);
        cJSON_AddNumberToObject(night, "humidity_pct", w->nighttime_humidity_pct);
        cJSON_AddNumberToObject(night, "precipitation_mm", w->nighttime_precip_mm);
        cJSON_AddNumberToObject(night, "precipitation_probability_pct",
                               w->nighttime_precip_probability_pct);
        cJSON_AddNumberToObject(night, "wind_speed_mps", w->nighttime_wind_speed_mps);
        cJSON_AddNumberToObject(night, "wind_scale", w->nighttime_wind_scale);

        if (!cJSON_AddItemToArray(items, item)) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            weather_snapshot_release(snap);
            return ESP_ERR_NO_MEM;
        }
    }

    return write_snapshot_json(root, snap, output, output_size);
}

static esp_err_t cap_weather_get_alerts_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size)
{
    (void)ctx;

    bool include_details = false;
    cJSON *input = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!input || !cJSON_IsObject(input)) {
        cJSON_Delete(input);
        return finish_error(make_error("invalid input json", ESP_ERR_INVALID_ARG),
                            ESP_ERR_INVALID_ARG, output, output_size);
    }

    cJSON *details = cJSON_GetObjectItemCaseSensitive(input, "include_details");
    if (details) {
        if (!cJSON_IsBool(details)) {
            cJSON_Delete(input);
            return finish_error(make_error("include_details must be boolean",
                                            ESP_ERR_INVALID_ARG),
                                ESP_ERR_INVALID_ARG, output, output_size);
        }
        include_details = cJSON_IsTrue(details);
    }
    cJSON_Delete(input);

    weather_snapshot_t *snap = NULL;
    weather_service_status_t status;
    esp_err_t err = get_snapshot_or_error(&snap, &status, WEATHER_SNAPSHOT_ALERTS,
                                          output, output_size);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!root || !items) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        weather_snapshot_release(snap);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    add_cache_meta(root, &status, snap);
    if (snap->alert_attribution[0]) {
        cJSON_AddStringToObject(root, "alert_attribution", snap->alert_attribution);
    }
    cJSON_AddNumberToObject(root, "count", (double)snap->alert_count);
    if (!cJSON_AddItemToObject(root, "alerts", items)) {
        cJSON_Delete(items);
        cJSON_Delete(root);
        weather_snapshot_release(snap);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < snap->alert_count; i++) {
        const weather_alert_t *a = &snap->alerts[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            weather_snapshot_release(snap);
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(item, "id", a->id);
        cJSON_AddStringToObject(item, "sender", a->sender);
        cJSON_AddStringToObject(item, "event", a->event_name);
        cJSON_AddStringToObject(item, "event_code", a->event_code);
        cJSON_AddStringToObject(item, "severity", a->severity);
        cJSON_AddStringToObject(item, "urgency", a->urgency);
        cJSON_AddStringToObject(item, "certainty", a->certainty);
        cJSON_AddStringToObject(item, "color", a->color);
        cJSON_AddStringToObject(item, "issued_time", a->issued_time);
        cJSON_AddStringToObject(item, "effective_time", a->effective_time);
        cJSON_AddStringToObject(item, "onset_time", a->onset_time);
        cJSON_AddStringToObject(item, "expire_time", a->expire_time);
        cJSON_AddStringToObject(item, "headline", a->headline);
        if (include_details) {
            cJSON_AddStringToObject(item, "description", a->description);
            cJSON_AddStringToObject(item, "instruction", a->instruction);
        }
        if (!cJSON_AddItemToArray(items, item)) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            weather_snapshot_release(snap);
            return ESP_ERR_NO_MEM;
        }
    }

    return write_snapshot_json(root, snap, output, output_size);
}

static esp_err_t cap_weather_get_status_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size)
{
    (void)input_json;
    (void)ctx;

    weather_service_status_t status = {0};
    weather_snapshot_t *snap = NULL;
    esp_err_t err = weather_snapshot_acquire(&snap);
    if (err != ESP_OK) {
        return finish_error(make_error("weather snapshot scratch unavailable", err),
                            err, output, output_size);
    }

    weather_service_get_status(&status);
    (void)weather_service_get_snapshot(snap);

    time_t now = time(NULL);
    int64_t age_sec = -1;
    if (status.last_success_at > 0 && now >= status.last_success_at) {
        age_sec = (int64_t)(now - status.last_success_at);
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        weather_snapshot_release(snap);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "initialized", status.initialized);
    cJSON_AddBoolToObject(root, "running", status.running);
    cJSON_AddBoolToObject(root, "network_online", status.network_online);
    cJSON_AddBoolToObject(root, "provider_registered", status.provider_registered);
    cJSON_AddBoolToObject(root, "provider_configured", status.provider_configured);
    cJSON_AddStringToObject(root, "provider", status.provider);
    cJSON_AddNumberToObject(root, "refresh_interval_minutes",
                           (double)(status.refresh_interval_ms / 60000U));
    cJSON_AddStringToObject(root, "last_error", esp_err_to_name(status.last_error));
    cJSON_AddNumberToObject(root, "last_attempt_epoch", (double)status.last_attempt_at);
    cJSON_AddNumberToObject(root, "last_success_epoch", (double)status.last_success_at);
    cJSON_AddNumberToObject(root, "cache_age_sec", (double)age_sec);
    cJSON_AddNumberToObject(root, "valid_mask", (double)snap->valid_mask);
    cJSON_AddNumberToObject(root, "hourly_count", (double)snap->hourly_count);
    cJSON_AddNumberToObject(root, "daily_count", (double)snap->daily_count);
    cJSON_AddNumberToObject(root, "alert_count", (double)snap->alert_count);

    return write_snapshot_json(root, snap, output, output_size);
}

static const claw_cap_descriptor_t s_weather_descriptors[] = {
    {
        .id = "weather_get_current",
        .name = "weather_get_current",
        .family = "weather",
        .description =
            "Read cached current outdoor weather from the station's configured provider. "
            "Use for current temperature, feels-like temperature, humidity, pressure, wind, "
            "precipitation, visibility, cloud cover or UV. Reads local cache only and does not "
            "make an extra network request. Never fabricate weather values.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_weather_get_current_execute,
    },
    {
        .id = "weather_get_hourly",
        .name = "weather_get_hourly",
        .family = "weather",
        .description =
            "Read cached hourly outdoor forecast. Use for the next few hours, rain timing, "
            "temperature changes or wind changes. Default 6 hours, maximum 24. Reads cache only.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{\"hours\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":24}}}",
        .execute = cap_weather_get_hourly_execute,
    },
    {
        .id = "weather_get_daily",
        .name = "weather_get_daily",
        .family = "weather",
        .description =
            "Read cached daily outdoor forecast for up to 7 days. Use for today/tomorrow, "
            "high/low temperature, day/night conditions, rain probability, sunrise/sunset "
            "and multi-day planning. Reads cache only.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{\"days\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":7}}}",
        .execute = cap_weather_get_daily_execute,
    },
    {
        .id = "weather_get_alerts",
        .name = "weather_get_alerts",
        .family = "weather",
        .description =
            "Read cached active weather alerts. Use whenever the user asks about warnings, "
            "severe weather, storms, heavy rain, typhoons or weather safety. Set "
            "include_details=true only when full alert instructions are needed.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{\"include_details\":{\"type\":\"boolean\"}}}",
        .execute = cap_weather_get_alerts_execute,
    },
    {
        .id = "weather_get_status",
        .name = "weather_get_status",
        .family = "weather",
        .description =
            "Get weather service health, provider configuration, network state, cache age "
            "and last refresh result. Use to diagnose unavailable or stale weather data.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_weather_get_status_execute,
    },
};

static const claw_cap_group_t s_weather_group = {
    .group_id = "cap_weather_station",
    .descriptors = s_weather_descriptors,
    .descriptor_count = sizeof(s_weather_descriptors) / sizeof(s_weather_descriptors[0]),
    .group_init = weather_runtime_init,
};

esp_err_t cap_weather_station_register_group(void)
{
    if (claw_cap_group_exists(s_weather_group.group_id)) {
        return ESP_OK;
    }

    esp_err_t err = claw_cap_register_group(&s_weather_group);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register group failed: %s", esp_err_to_name(err));
    }
    return err;
}
