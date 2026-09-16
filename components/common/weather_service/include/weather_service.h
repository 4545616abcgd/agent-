/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEATHER_MAX_HOURLY        24U
#define WEATHER_MAX_DAILY         7U
#define WEATHER_MAX_ALERTS        4U

#define WEATHER_SNAPSHOT_CURRENT   (1U << 0)
#define WEATHER_SNAPSHOT_HOURLY    (1U << 1)
#define WEATHER_SNAPSHOT_DAILY     (1U << 2)
#define WEATHER_SNAPSHOT_ALERTS    (1U << 3)

#define WEATHER_CONDITION_TEXT_MAX 48U
#define WEATHER_CONDITION_CODE_MAX 12U
#define WEATHER_TIME_TEXT_MAX      40U
#define WEATHER_ATTRIBUTION_MAX    320U
#define WEATHER_ALERT_HEADLINE_MAX 160U
#define WEATHER_ALERT_DESC_MAX     640U
#define WEATHER_ALERT_INST_MAX     768U

typedef struct {
    bool valid;
    time_t fetched_at;
    char condition_text[WEATHER_CONDITION_TEXT_MAX];
    char condition_code[WEATHER_CONDITION_CODE_MAX];
    float temperature_c;
    float feels_like_c;
    float humidity_pct;
    float pressure_hpa;
    float precipitation_mm;
    float precipitation_intensity_mm_h;
    float visibility_km;
    float dew_point_c;
    float cloud_cover_pct;
    float uv_index;
    float wind_speed_mps;
    float wind_gust_mps;
    int wind_direction_deg;
    int wind_scale;
    char wind_compass[8];
    char precipitation_type[12];
} weather_current_t;

typedef struct {
    char forecast_time[WEATHER_TIME_TEXT_MAX];
    char condition_text[WEATHER_CONDITION_TEXT_MAX];
    char condition_code[WEATHER_CONDITION_CODE_MAX];
    float temperature_c;
    float feels_like_c;
    float humidity_pct;
    float pressure_hpa;
    float precipitation_mm;
    float precipitation_probability_pct;
    float wind_speed_mps;
    float wind_gust_mps;
    int wind_direction_deg;
    int wind_scale;
    char wind_compass[8];
} weather_hourly_t;

typedef struct {
    char forecast_start[WEATHER_TIME_TEXT_MAX];
    char forecast_end[WEATHER_TIME_TEXT_MAX];
    char sunrise[WEATHER_TIME_TEXT_MAX];
    char sunset[WEATHER_TIME_TEXT_MAX];
    char moon_phase[24];
    float temperature_max_c;
    float temperature_min_c;
    float temperature_avg_c;
    float uv_index_max;

    char daytime_text[WEATHER_CONDITION_TEXT_MAX];
    char daytime_code[WEATHER_CONDITION_CODE_MAX];
    float daytime_humidity_pct;
    float daytime_precip_mm;
    float daytime_precip_probability_pct;
    float daytime_wind_speed_mps;
    int daytime_wind_scale;

    char nighttime_text[WEATHER_CONDITION_TEXT_MAX];
    char nighttime_code[WEATHER_CONDITION_CODE_MAX];
    float nighttime_humidity_pct;
    float nighttime_precip_mm;
    float nighttime_precip_probability_pct;
    float nighttime_wind_speed_mps;
    int nighttime_wind_scale;
} weather_daily_t;

typedef struct {
    char id[40];
    char sender[64];
    char event_name[48];
    char event_code[16];
    char severity[16];
    char urgency[16];
    char certainty[16];
    char color[16];
    char issued_time[WEATHER_TIME_TEXT_MAX];
    char effective_time[WEATHER_TIME_TEXT_MAX];
    char onset_time[WEATHER_TIME_TEXT_MAX];
    char expire_time[WEATHER_TIME_TEXT_MAX];
    char headline[WEATHER_ALERT_HEADLINE_MAX];
    char description[WEATHER_ALERT_DESC_MAX];
    char instruction[WEATHER_ALERT_INST_MAX];
} weather_alert_t;

typedef struct {
    uint32_t valid_mask;
    char provider[24];
    char attribution[WEATHER_ATTRIBUTION_MAX];
    char alert_attribution[WEATHER_ATTRIBUTION_MAX];

    weather_current_t current;

    weather_hourly_t hourly[WEATHER_MAX_HOURLY];
    size_t hourly_count;

    weather_daily_t daily[WEATHER_MAX_DAILY];
    size_t daily_count;

    weather_alert_t alerts[WEATHER_MAX_ALERTS];
    size_t alert_count;
} weather_snapshot_t;

typedef struct {
    bool initialized;
    bool running;
    bool network_online;
    bool provider_registered;
    bool provider_configured;
    char provider[24];
    time_t last_attempt_at;
    time_t last_success_at;
    esp_err_t last_error;
    uint32_t refresh_interval_ms;
} weather_service_status_t;

typedef struct {
    uint32_t refresh_interval_ms;
} weather_service_config_t;

typedef struct {
    const char *name;
    void *ctx;
    bool (*is_configured)(void *ctx);
    esp_err_t (*fetch)(void *ctx, weather_snapshot_t *out);
} weather_provider_t;

esp_err_t weather_service_init(const weather_service_config_t *config);
esp_err_t weather_service_register_provider(const weather_provider_t *provider);
esp_err_t weather_service_start(void);
esp_err_t weather_service_stop(void);

void weather_service_set_network_online(bool online);
esp_err_t weather_service_request_refresh(void);
esp_err_t weather_service_refresh_and_wait(uint32_t timeout_ms);

esp_err_t weather_service_get_snapshot(weather_snapshot_t *out);
void weather_service_get_status(weather_service_status_t *out);

#ifdef __cplusplus
}
#endif
