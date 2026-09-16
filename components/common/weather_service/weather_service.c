/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "weather_service.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "weather_service";

#define WEATHER_EVT_REFRESH BIT0
#define WEATHER_EVT_STOP    BIT1
#define WEATHER_EVT_DONE    BIT2

typedef struct {
    SemaphoreHandle_t mutex;
    EventGroupHandle_t events;
    TaskHandle_t task;
    weather_provider_t provider;
    weather_snapshot_t snapshot;
    weather_service_status_t status;
} weather_service_ctx_t;

static weather_service_ctx_t s_weather;

static void status_set_error(esp_err_t err)
{
    if (!s_weather.mutex) {
        return;
    }
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    s_weather.status.last_error = err;
    xSemaphoreGive(s_weather.mutex);
}

static void merge_snapshot_locked(const weather_snapshot_t *src)
{
    if (!src) {
        return;
    }

    if (src->provider[0]) {
        strlcpy(s_weather.snapshot.provider, src->provider, sizeof(s_weather.snapshot.provider));
    }
    if (src->attribution[0]) {
        strlcpy(s_weather.snapshot.attribution, src->attribution,
                sizeof(s_weather.snapshot.attribution));
    }
    if (src->alert_attribution[0]) {
        strlcpy(s_weather.snapshot.alert_attribution, src->alert_attribution,
                sizeof(s_weather.snapshot.alert_attribution));
    }

    if (src->valid_mask & WEATHER_SNAPSHOT_CURRENT) {
        s_weather.snapshot.current = src->current;
        s_weather.snapshot.valid_mask |= WEATHER_SNAPSHOT_CURRENT;
    }

    if (src->valid_mask & WEATHER_SNAPSHOT_HOURLY) {
        s_weather.snapshot.hourly_count = src->hourly_count;
        if (src->hourly_count > 0) {
            memcpy(s_weather.snapshot.hourly, src->hourly,
                   src->hourly_count * sizeof(src->hourly[0]));
        }
        s_weather.snapshot.valid_mask |= WEATHER_SNAPSHOT_HOURLY;
    }

    if (src->valid_mask & WEATHER_SNAPSHOT_DAILY) {
        s_weather.snapshot.daily_count = src->daily_count;
        if (src->daily_count > 0) {
            memcpy(s_weather.snapshot.daily, src->daily,
                   src->daily_count * sizeof(src->daily[0]));
        }
        s_weather.snapshot.valid_mask |= WEATHER_SNAPSHOT_DAILY;
    }

    /*
     * A successful alert request may legitimately return zero active alerts.
     * The ALERTS validity bit distinguishes that from a failed alert request.
     */
    if (src->valid_mask & WEATHER_SNAPSHOT_ALERTS) {
        s_weather.snapshot.alert_count = src->alert_count;
        if (src->alert_count > 0) {
            memcpy(s_weather.snapshot.alerts, src->alerts,
                   src->alert_count * sizeof(src->alerts[0]));
        }
        s_weather.snapshot.valid_mask |= WEATHER_SNAPSHOT_ALERTS;
    }
}

static esp_err_t refresh_once(void)
{
    weather_provider_t provider = {0};
    bool network_online = false;

    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    provider = s_weather.provider;
    network_online = s_weather.status.network_online;
    s_weather.status.last_attempt_at = time(NULL);
    xSemaphoreGive(s_weather.mutex);

    if (!network_online) {
        status_set_error(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if (!provider.fetch || !provider.name) {
        status_set_error(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if (provider.is_configured && !provider.is_configured(provider.ctx)) {
        status_set_error(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }

    weather_snapshot_t *incoming = calloc(1, sizeof(*incoming));
    if (!incoming) {
        status_set_error(ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = provider.fetch(provider.ctx, incoming);
    if (err == ESP_OK) {
        xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
        merge_snapshot_locked(incoming);
        s_weather.status.last_success_at = time(NULL);
        s_weather.status.last_error = ESP_OK;
        xSemaphoreGive(s_weather.mutex);
        ESP_LOGI(TAG, "weather refresh succeeded provider=%s", provider.name);
    } else {
        status_set_error(err);
        ESP_LOGW(TAG, "weather refresh failed provider=%s: %s",
                 provider.name, esp_err_to_name(err));
    }

    free(incoming);
    return err;
}

static void weather_task(void *arg)
{
    (void)arg;

    while (1) {
        uint32_t wait_ms = CONFIG_WEATHER_SERVICE_REFRESH_MINUTES * 60U * 1000U;
        xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
        if (s_weather.status.refresh_interval_ms > 0) {
            wait_ms = s_weather.status.refresh_interval_ms;
        }
        xSemaphoreGive(s_weather.mutex);

        EventBits_t bits = xEventGroupWaitBits(
            s_weather.events,
            WEATHER_EVT_REFRESH | WEATHER_EVT_STOP,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(wait_ms));

        if (bits & WEATHER_EVT_STOP) {
            break;
        }

        (void)refresh_once();
        xEventGroupSetBits(s_weather.events, WEATHER_EVT_DONE);
    }

    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    s_weather.status.running = false;
    s_weather.task = NULL;
    xSemaphoreGive(s_weather.mutex);
    vTaskDelete(NULL);
}

esp_err_t weather_service_init(const weather_service_config_t *config)
{
    if (s_weather.status.initialized) {
        return ESP_OK;
    }

    memset(&s_weather, 0, sizeof(s_weather));
    s_weather.mutex = xSemaphoreCreateMutex();
    s_weather.events = xEventGroupCreate();
    if (!s_weather.mutex || !s_weather.events) {
        if (s_weather.mutex) {
            vSemaphoreDelete(s_weather.mutex);
        }
        if (s_weather.events) {
            vEventGroupDelete(s_weather.events);
        }
        memset(&s_weather, 0, sizeof(s_weather));
        return ESP_ERR_NO_MEM;
    }

    s_weather.status.initialized = true;
    s_weather.status.refresh_interval_ms =
        (config && config->refresh_interval_ms > 0)
            ? config->refresh_interval_ms
            : CONFIG_WEATHER_SERVICE_REFRESH_MINUTES * 60U * 1000U;
    s_weather.status.last_error = ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "initialized refresh=%u min",
             (unsigned)(s_weather.status.refresh_interval_ms / 60000U));
    return ESP_OK;
}

esp_err_t weather_service_register_provider(const weather_provider_t *provider)
{
    if (!s_weather.status.initialized || !s_weather.mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!provider || !provider->name || !provider->name[0] || !provider->fetch) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    if (s_weather.status.running) {
        xSemaphoreGive(s_weather.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_weather.provider = *provider;
    s_weather.status.provider_registered = true;
    strlcpy(s_weather.status.provider, provider->name, sizeof(s_weather.status.provider));
    s_weather.status.provider_configured =
        provider->is_configured ? provider->is_configured(provider->ctx) : true;
    xSemaphoreGive(s_weather.mutex);

    ESP_LOGI(TAG, "provider registered: %s configured=%d",
             provider->name, s_weather.status.provider_configured);
    return ESP_OK;
}

esp_err_t weather_service_start(void)
{
    if (!s_weather.status.initialized || !s_weather.mutex || !s_weather.events) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    if (s_weather.status.running) {
        xSemaphoreGive(s_weather.mutex);
        return ESP_OK;
    }
    if (!s_weather.status.provider_registered) {
        xSemaphoreGive(s_weather.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_weather.status.running = true;
    xSemaphoreGive(s_weather.mutex);

    BaseType_t ok = xTaskCreate(
        weather_task,
        "weather",
        CONFIG_WEATHER_SERVICE_TASK_STACK_SIZE,
        NULL,
        CONFIG_WEATHER_SERVICE_TASK_PRIORITY,
        &s_weather.task);
    if (ok != pdPASS) {
        xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
        s_weather.status.running = false;
        xSemaphoreGive(s_weather.mutex);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t weather_service_stop(void)
{
    if (!s_weather.status.initialized || !s_weather.events) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    bool running = s_weather.status.running;
    xSemaphoreGive(s_weather.mutex);
    if (!running) {
        return ESP_OK;
    }

    xEventGroupSetBits(s_weather.events, WEATHER_EVT_STOP);
    return ESP_OK;
}

void weather_service_set_network_online(bool online)
{
    if (!s_weather.status.initialized || !s_weather.mutex) {
        return;
    }

    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    bool changed = (s_weather.status.network_online != online);
    s_weather.status.network_online = online;
    if (s_weather.provider.is_configured) {
        s_weather.status.provider_configured =
            s_weather.provider.is_configured(s_weather.provider.ctx);
    }
    xSemaphoreGive(s_weather.mutex);

    if (online && changed && s_weather.events) {
        xEventGroupSetBits(s_weather.events, WEATHER_EVT_REFRESH);
    }
}

esp_err_t weather_service_request_refresh(void)
{
    if (!s_weather.status.initialized || !s_weather.events) {
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(s_weather.events, WEATHER_EVT_REFRESH);
    return ESP_OK;
}

esp_err_t weather_service_refresh_and_wait(uint32_t timeout_ms)
{
    if (!s_weather.status.initialized || !s_weather.events) {
        return ESP_ERR_INVALID_STATE;
    }

    xEventGroupClearBits(s_weather.events, WEATHER_EVT_DONE);
    xEventGroupSetBits(s_weather.events, WEATHER_EVT_REFRESH);

    EventBits_t bits = xEventGroupWaitBits(
        s_weather.events,
        WEATHER_EVT_DONE,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    if (!(bits & WEATHER_EVT_DONE)) {
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    esp_err_t err = s_weather.status.last_error;
    xSemaphoreGive(s_weather.mutex);
    return err;
}

esp_err_t weather_service_get_snapshot(weather_snapshot_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_weather.status.initialized || !s_weather.mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    *out = s_weather.snapshot;
    xSemaphoreGive(s_weather.mutex);
    return ESP_OK;
}

void weather_service_get_status(weather_service_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    if (!s_weather.status.initialized || !s_weather.mutex) {
        return;
    }

    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    *out = s_weather.status;
    if (s_weather.provider.is_configured) {
        out->provider_configured = s_weather.provider.is_configured(s_weather.provider.ctx);
    }
    xSemaphoreGive(s_weather.mutex);
}
