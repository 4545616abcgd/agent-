/* Exercise the production weather refresh path with clock/provider stubs. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "weather_service.h"

static time_t now;
static unsigned fetches;
static bool configured = true;
static esp_err_t fetch_error = ESP_OK;
static time_t test_time(time_t *out) { if (out) *out = now; return now; }
static size_t strlcpy(char *dest, const char *source, size_t capacity)
{
    size_t length = strlen(source);
    if (capacity) {
        size_t copy = length < capacity - 1 ? length : capacity - 1;
        memcpy(dest, source, copy);
        dest[copy] = 0;
    }
    return length;
}
#define time test_time
#include "../../../../../components/common/weather_service/weather_service.c"
#undef time

static bool is_configured(void *ctx) { (void)ctx; return configured; }
static esp_err_t fetch(void *ctx, weather_snapshot_t *out)
{
    (void)ctx;
    ++fetches;
    out->valid_mask = WEATHER_SNAPSHOT_CURRENT;
    out->current.valid = true;
    out->current.fetched_at = now;
    out->current.temperature_c = 26;
    return fetch_error;
}

int main(void)
{
    assert(weather_service_init(NULL) == ESP_OK);
    weather_provider_t provider = {.name = "test", .is_configured = is_configured, .fetch = fetch};
    assert(weather_service_register_provider(&provider) == ESP_OK);
    weather_service_set_network_online(true);
    now = 13;
    assert(refresh_once() == ESP_ERR_INVALID_STATE && fetches == 0);
    assert(s_weather.status.last_attempt_at == 0 && s_weather.status.last_success_at == 0);
    now = WEATHER_MIN_VALID_EPOCH - 1;
    assert(refresh_once() == ESP_ERR_INVALID_STATE && fetches == 0);
    now = WEATHER_MIN_VALID_EPOCH;
    assert(refresh_once() == ESP_OK && fetches == 1);
    assert(s_weather.status.last_success_at == now && s_weather.snapshot.current.fetched_at == now);
    weather_service_set_network_online(false);
    assert(refresh_once() == ESP_ERR_INVALID_STATE && fetches == 1);
    weather_service_set_network_online(true);
    configured = false;
    assert(refresh_once() == ESP_ERR_INVALID_STATE && fetches == 1);
    configured = true;
    fetch_error = ESP_FAIL;
    now += 1800;
    assert(refresh_once() == ESP_FAIL && fetches == 2);
    assert(s_weather.snapshot.current.fetched_at == WEATHER_MIN_VALID_EPOCH);
    fetch_error = ESP_OK;
    assert(refresh_once() == ESP_OK && fetches == 3);
    assert(s_weather.status.last_success_at == now && s_weather.snapshot.current.fetched_at == now);
    puts("PASS: pre-SNTP fetch suppressed, valid epoch recorded, offline/config failures and old cache preserved");
    return 0;
}
