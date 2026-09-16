/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEATHER_QW_HOST_MAX 128
#define WEATHER_QW_KEY_MAX  160

typedef struct {
    char api_host[WEATHER_QW_HOST_MAX];
    char api_key[WEATHER_QW_KEY_MAX];
    double latitude;
    double longitude;
    bool location_set;
} weather_qweather_config_t;

esp_err_t weather_provider_qweather_register(void);

esp_err_t weather_provider_qweather_get_config(weather_qweather_config_t *out);
esp_err_t weather_provider_qweather_set_host(const char *host);
esp_err_t weather_provider_qweather_set_api_key(const char *api_key);
esp_err_t weather_provider_qweather_clear_api_key(void);
esp_err_t weather_provider_qweather_set_location(double latitude, double longitude);

#ifdef __cplusplus
}
#endif
