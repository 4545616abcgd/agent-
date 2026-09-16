/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "weather_provider_qweather.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "settings_store.h"
#include "weather_service.h"
#include "zlib.h"

static const char *TAG = "weather_qw";

#define QW_SETTING_HOST "qw_host"
#define QW_SETTING_KEY  "qw_key"
#define QW_SETTING_LAT  "qw_lat"
#define QW_SETTING_LON  "qw_lon"

#define QW_URL_MAX                  320U
#define QW_HTTP_TIMEOUT_MS          15000
#define QW_BODY_INITIAL_CAP         4096U
#define QW_BODY_MAX_COMPRESSED      (128U * 1024U)
#define QW_BODY_MAX_DECOMPRESSED    (320U * 1024U)

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    bool psram;
    bool gzip;
    esp_err_t error;
} qw_http_buffer_t;

static void *weather_alloc(size_t size, bool *psram)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        if (psram) {
            *psram = true;
        }
        return p;
    }

    p = malloc(size);
    if (psram) {
        *psram = false;
    }
    return p;
}

static void *weather_realloc(void *ptr, size_t size, bool psram)
{
    if (psram) {
        return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return realloc(ptr, size);
}

static esp_err_t buffer_reserve(qw_http_buffer_t *buf, size_t needed)
{
    if (!buf) {
        return ESP_ERR_INVALID_ARG;
    }
    if (needed <= buf->cap) {
        return ESP_OK;
    }
    if (needed > QW_BODY_MAX_COMPRESSED) {
        return ESP_ERR_NO_MEM;
    }

    size_t new_cap = buf->cap ? buf->cap : QW_BODY_INITIAL_CAP;
    while (new_cap < needed) {
        new_cap *= 2U;
        if (new_cap > QW_BODY_MAX_COMPRESSED) {
            new_cap = QW_BODY_MAX_COMPRESSED;
            break;
        }
    }

    uint8_t *grown = NULL;
    if (!buf->data) {
        grown = weather_alloc(new_cap, &buf->psram);
    } else {
        grown = weather_realloc(buf->data, new_cap, buf->psram);
    }
    if (!grown) {
        return ESP_ERR_NO_MEM;
    }
    buf->data = grown;
    buf->cap = new_cap;
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    qw_http_buffer_t *buf = evt ? (qw_http_buffer_t *)evt->user_data : NULL;
    if (!buf) {
        return ESP_OK;
    }

    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        /*
         * http_reuse may transparently reconnect and retry a stale pooled
         * connection. If the first attempt delivered partial data, discard it
         * before the retried response arrives.
         */
        if (buf->len > 0) {
            buf->len = 0;
            buf->gzip = false;
            buf->error = ESP_OK;
            if (buf->data) {
                buf->data[0] = '\0';
            }
        }
        break;

    case HTTP_EVENT_ON_HEADER:
        if (evt->header_key && evt->header_value &&
            strcasecmp(evt->header_key, "Content-Encoding") == 0 &&
            strcasecmp(evt->header_value, "gzip") == 0) {
            buf->gzip = true;
        }
        break;

    case HTTP_EVENT_ON_DATA:
        if (evt->data && evt->data_len > 0) {
            size_t needed = buf->len + (size_t)evt->data_len + 1U;
            esp_err_t err = buffer_reserve(buf, needed);
            if (err != ESP_OK) {
                buf->error = err;
                return err;
            }
            memcpy(buf->data + buf->len, evt->data, (size_t)evt->data_len);
            buf->len += (size_t)evt->data_len;
            buf->data[buf->len] = '\0';
        }
        break;

    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t gunzip_body(const uint8_t *src, size_t src_len, char **out_text)
{
    if (!src || src_len == 0 || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    z_stream zs = {0};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) {
        return ESP_FAIL;
    }

    size_t cap = src_len * 4U;
    if (cap < QW_BODY_INITIAL_CAP) {
        cap = QW_BODY_INITIAL_CAP;
    }
    if (cap > QW_BODY_MAX_DECOMPRESSED) {
        cap = QW_BODY_MAX_DECOMPRESSED;
    }

    bool psram = false;
    uint8_t *out = weather_alloc(cap + 1U, &psram);
    if (!out) {
        inflateEnd(&zs);
        return ESP_ERR_NO_MEM;
    }

    zs.next_in = (Bytef *)src;
    zs.avail_in = (uInt)src_len;

    size_t produced = 0;
    int zret = Z_OK;
    while (1) {
        if (produced == cap) {
            if (cap >= QW_BODY_MAX_DECOMPRESSED) {
                free(out);
                inflateEnd(&zs);
                return ESP_ERR_NO_MEM;
            }
            size_t new_cap = cap * 2U;
            if (new_cap > QW_BODY_MAX_DECOMPRESSED) {
                new_cap = QW_BODY_MAX_DECOMPRESSED;
            }
            uint8_t *grown = weather_realloc(out, new_cap + 1U, psram);
            if (!grown) {
                free(out);
                inflateEnd(&zs);
                return ESP_ERR_NO_MEM;
            }
            out = grown;
            cap = new_cap;
        }

        zs.next_out = out + produced;
        zs.avail_out = (uInt)(cap - produced);
        zret = inflate(&zs, Z_NO_FLUSH);
        produced = cap - zs.avail_out;

        if (zret == Z_STREAM_END) {
            break;
        }
        if (zret == Z_OK) {
            continue;
        }
        if (zret == Z_BUF_ERROR && zs.avail_out == 0) {
            continue;
        }

        free(out);
        inflateEnd(&zs);
        return ESP_ERR_INVALID_RESPONSE;
    }

    out[produced] = '\0';
    inflateEnd(&zs);
    *out_text = (char *)out;
    return ESP_OK;
}

static esp_err_t qweather_http_get(const char *url, const char *api_key, char **out_json)
{
    if (!url || !api_key || !api_key[0] || !out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;

    qw_http_buffer_t body = {0};
    body.error = ESP_OK;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = QW_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &body,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "X-QW-Api-Key", api_key);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "gzip");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err == ESP_OK && body.error != ESP_OK) {
        err = body.error;
    }
    if (err == ESP_OK && status != 200) {
        ESP_LOGW(TAG, "HTTP status=%d url=%s", status, url);
        err = (status == 401 || status == 403) ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }

    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        free(body.data);
        return err;
    }
    if (!body.data || body.len == 0) {
        free(body.data);
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool magic_gzip = body.len >= 2 && body.data[0] == 0x1f && body.data[1] == 0x8b;
    if (body.gzip || magic_gzip) {
        char *json = NULL;
        err = gunzip_body(body.data, body.len, &json);
        free(body.data);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gzip decode failed: %s", esp_err_to_name(err));
            return err;
        }
        *out_json = json;
        return ESP_OK;
    }

    *out_json = (char *)body.data;
    return ESP_OK;
}

static const cJSON *obj(const cJSON *parent, const char *key)
{
    const cJSON *item = parent ? cJSON_GetObjectItemCaseSensitive((cJSON *)parent, key) : NULL;
    return cJSON_IsObject(item) ? item : NULL;
}

static const cJSON *arr(const cJSON *parent, const char *key)
{
    const cJSON *item = parent ? cJSON_GetObjectItemCaseSensitive((cJSON *)parent, key) : NULL;
    return cJSON_IsArray(item) ? item : NULL;
}

static float num(const cJSON *parent, const char *key, float fallback)
{
    const cJSON *item = parent ? cJSON_GetObjectItemCaseSensitive((cJSON *)parent, key) : NULL;
    return cJSON_IsNumber(item) ? (float)item->valuedouble : fallback;
}

static float nested_value(const cJSON *parent, const char *key, float fallback)
{
    return num(obj(parent, key), "value", fallback);
}

static void utf8_trim_incomplete_tail(char *text)
{
    if (!text) {
        return;
    }

    size_t len = strlen(text);
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)text[i];
        size_t need = 1;

        if ((c & 0x80U) == 0) {
            need = 1;
        } else if ((c & 0xE0U) == 0xC0U) {
            need = 2;
        } else if ((c & 0xF0U) == 0xE0U) {
            need = 3;
        } else if ((c & 0xF8U) == 0xF0U) {
            need = 4;
        } else {
            text[i] = '\0';
            return;
        }

        if (i + need > len) {
            text[i] = '\0';
            return;
        }
        for (size_t j = 1; j < need; j++) {
            if ((((unsigned char)text[i + j]) & 0xC0U) != 0x80U) {
                text[i] = '\0';
                return;
            }
        }
        i += need;
    }
}

static void copy_text_value(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }

    size_t src_len = strlen(src);
    strlcpy(dst, src, dst_size);
    if (src_len >= dst_size) {
        utf8_trim_incomplete_tail(dst);
    }
}

static void copy_string(const cJSON *parent, const char *key, char *dst, size_t dst_size)
{
    const cJSON *item = parent ? cJSON_GetObjectItemCaseSensitive((cJSON *)parent, key) : NULL;
    copy_text_value((cJSON_IsString(item) && item->valuestring) ? item->valuestring : NULL,
                    dst, dst_size);
}

static void copy_condition(const cJSON *parent, char *text, size_t text_size,
                           char *code, size_t code_size)
{
    const cJSON *condition = obj(parent, "condition");
    copy_string(condition, "text", text, text_size);
    copy_string(condition, "code", code, code_size);
}

static void copy_attributions(const cJSON *root, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';

    const cJSON *metadata = obj(root, "metadata");
    const cJSON *attributions = arr(metadata, "attributions");
    if (!attributions) {
        return;
    }

    int count = cJSON_GetArraySize((cJSON *)attributions);
    for (int i = 0; i < count; i++) {
        const cJSON *item = cJSON_GetArrayItem((cJSON *)attributions, i);
        if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
            continue;
        }

        size_t used = strlen(dst);
        if (used > 0 && used + 3U < dst_size) {
            strlcat(dst, " | ", dst_size);
            used = strlen(dst);
        }
        if (used + 1U < dst_size) {
            strlcat(dst, item->valuestring, dst_size);
        }
        if (strlen(dst) + 1U >= dst_size) {
            utf8_trim_incomplete_tail(dst);
            break;
        }
    }
}

static bool parse_double_text(const char *text, double min_v, double max_v, double *out)
{
    if (!text || !text[0] || !out) {
        return false;
    }

    char *end = NULL;
    double value = strtod(text, &end);
    if (!end || *end != '\0' || !(value >= min_v && value <= max_v)) {
        return false;
    }
    *out = value;
    return true;
}

static esp_err_t normalize_host(const char *host, char *out, size_t out_size)
{
    if (!host || !host[0] || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (isspace((unsigned char)*host)) {
        host++;
    }
    if (strncasecmp(host, "https://", 8) == 0) {
        host += 8;
    } else if (strncasecmp(host, "http://", 7) == 0) {
        host += 7;
    }

    size_t len = strcspn(host, "/ \t\r\n");
    if (len == 0 || len >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out, host, len);
    out[len] = '\0';
    return strchr(out, '.') ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t weather_provider_qweather_get_config(weather_qweather_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    esp_err_t err = settings_store_get_string(QW_SETTING_HOST, out->api_host,
                                               sizeof(out->api_host), "");
    if (err != ESP_OK) {
        return err;
    }
    err = settings_store_get_string(QW_SETTING_KEY, out->api_key, sizeof(out->api_key), "");
    if (err != ESP_OK) {
        return err;
    }

    char lat[24] = {0};
    char lon[24] = {0};
    err = settings_store_get_string(QW_SETTING_LAT, lat, sizeof(lat), "");
    if (err != ESP_OK) {
        return err;
    }
    err = settings_store_get_string(QW_SETTING_LON, lon, sizeof(lon), "");
    if (err != ESP_OK) {
        return err;
    }

    double latitude = 0;
    double longitude = 0;
    if (parse_double_text(lat, -90.0, 90.0, &latitude) &&
        parse_double_text(lon, -180.0, 180.0, &longitude)) {
        out->latitude = latitude;
        out->longitude = longitude;
        out->location_set = true;
    }
    return ESP_OK;
}

esp_err_t weather_provider_qweather_set_host(const char *host)
{
    char normalized[WEATHER_QW_HOST_MAX] = {0};
    esp_err_t err = normalize_host(host, normalized, sizeof(normalized));
    if (err != ESP_OK) {
        return err;
    }
    return settings_store_set_string(QW_SETTING_HOST, normalized);
}

esp_err_t weather_provider_qweather_set_api_key(const char *api_key)
{
    if (!api_key || !api_key[0] || strlen(api_key) >= WEATHER_QW_KEY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return settings_store_set_string(QW_SETTING_KEY, api_key);
}

esp_err_t weather_provider_qweather_clear_api_key(void)
{
    return settings_store_erase_key(QW_SETTING_KEY);
}

esp_err_t weather_provider_qweather_set_location(double latitude, double longitude)
{
    if (!(latitude >= -90.0 && latitude <= 90.0) ||
        !(longitude >= -180.0 && longitude <= 180.0)) {
        return ESP_ERR_INVALID_ARG;
    }

    char lat[24];
    char lon[24];
    snprintf(lat, sizeof(lat), "%.4f", latitude);
    snprintf(lon, sizeof(lon), "%.4f", longitude);

    esp_err_t err = settings_store_set_string(QW_SETTING_LAT, lat);
    if (err != ESP_OK) {
        return err;
    }
    return settings_store_set_string(QW_SETTING_LON, lon);
}

static bool qweather_is_configured(void *ctx)
{
    (void)ctx;
    weather_qweather_config_t cfg = {0};
    if (weather_provider_qweather_get_config(&cfg) != ESP_OK) {
        return false;
    }
    return cfg.api_host[0] && cfg.api_key[0] && cfg.location_set;
}

static esp_err_t build_url(char *dst, size_t dst_size,
                           const weather_qweather_config_t *cfg,
                           const char *kind)
{
    if (!dst || !cfg || !kind) {
        return ESP_ERR_INVALID_ARG;
    }

    double lat = cfg->latitude;
    double lon = cfg->longitude;

    int n = -1;
    if (strcmp(kind, "current") == 0) {
        n = snprintf(dst, dst_size,
                     "https://%s/weather/v1/current/%.2f/%.2f?localTime=true&lang=zh",
                     cfg->api_host, lat, lon);
    } else if (strcmp(kind, "hourly") == 0) {
        n = snprintf(dst, dst_size,
                     "https://%s/weather/v1/hourly/%.2f/%.2f?hours=%u&localTime=true&lang=zh",
                     cfg->api_host, lat, lon, (unsigned)WEATHER_MAX_HOURLY);
    } else if (strcmp(kind, "daily") == 0) {
        n = snprintf(dst, dst_size,
                     "https://%s/weather/v1/daily/%.2f/%.2f?days=%u&localTime=true&lang=zh",
                     cfg->api_host, lat, lon, (unsigned)WEATHER_MAX_DAILY);
    } else if (strcmp(kind, "alerts") == 0) {
        n = snprintf(dst, dst_size,
                     "https://%s/weatheralert/v1/current/%.2f/%.2f?localTime=true&lang=zh",
                     cfg->api_host, lat, lon);
    }

    return (n > 0 && (size_t)n < dst_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t parse_current(const char *json, weather_snapshot_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    weather_current_t *w = &out->current;
    memset(w, 0, sizeof(*w));

    copy_condition(root, w->condition_text, sizeof(w->condition_text),
                   w->condition_code, sizeof(w->condition_code));
    w->temperature_c = nested_value(root, "temperature", 0);
    w->feels_like_c = nested_value(root, "feelsLike", 0);
    w->humidity_pct = num(root, "humidity", 0) * 100.0f;
    w->pressure_hpa = nested_value(root, "pressure", 0);
    w->visibility_km = nested_value(root, "visibility", 0) / 1000.0f;
    w->dew_point_c = nested_value(root, "dewPoint", 0);
    w->cloud_cover_pct = num(root, "cloudCover", 0) * 100.0f;
    w->uv_index = num(root, "uvIndex", 0);

    const cJSON *wind = obj(root, "wind");
    const cJSON *direction = obj(wind, "direction");
    const cJSON *speed = obj(wind, "speed");
    w->wind_direction_deg = (int)num(direction, "degree", 0);
    w->wind_scale = (int)num(wind, "scale", 0);
    w->wind_speed_mps = num(speed, "value", 0);
    copy_string(direction, "compass", w->wind_compass, sizeof(w->wind_compass));

    w->wind_gust_mps = nested_value(root, "windGust", 0);

    const cJSON *precip = obj(root, "precipitation");
    w->precipitation_mm = nested_value(precip, "amount", 0);
    w->precipitation_intensity_mm_h = nested_value(precip, "intensity", 0);
    copy_string(precip, "type", w->precipitation_type, sizeof(w->precipitation_type));

    w->fetched_at = time(NULL);
    w->valid = true;
    out->valid_mask |= WEATHER_SNAPSHOT_CURRENT;
    copy_attributions(root, out->attribution, sizeof(out->attribution));

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t parse_hourly(const char *json, weather_snapshot_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *hours = arr(root, "hours");
    if (!hours) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int count = cJSON_GetArraySize((cJSON *)hours);
    if (count > (int)WEATHER_MAX_HOURLY) {
        count = WEATHER_MAX_HOURLY;
    }

    out->hourly_count = 0;
    for (int i = 0; i < count; i++) {
        const cJSON *src = cJSON_GetArrayItem((cJSON *)hours, i);
        if (!cJSON_IsObject(src)) {
            continue;
        }

        weather_hourly_t *h = &out->hourly[out->hourly_count];
        memset(h, 0, sizeof(*h));

        copy_string(src, "forecastTime", h->forecast_time, sizeof(h->forecast_time));
        copy_condition(src, h->condition_text, sizeof(h->condition_text),
                       h->condition_code, sizeof(h->condition_code));
        h->temperature_c = nested_value(src, "temperature", 0);
        h->feels_like_c = nested_value(src, "feelsLike", 0);
        h->humidity_pct = num(src, "humidity", 0) * 100.0f;
        h->pressure_hpa = nested_value(src, "pressure", 0);

        const cJSON *precip = obj(src, "precipitation");
        h->precipitation_mm = nested_value(precip, "amount", 0);
        h->precipitation_probability_pct = num(precip, "probability", 0) * 100.0f;

        const cJSON *wind = obj(src, "wind");
        const cJSON *direction = obj(wind, "direction");
        h->wind_direction_deg = (int)num(direction, "degree", 0);
        h->wind_scale = (int)num(wind, "scale", 0);
        h->wind_speed_mps = nested_value(wind, "speed", 0);
        h->wind_gust_mps = nested_value(src, "windGust", 0);
        copy_string(direction, "compass", h->wind_compass, sizeof(h->wind_compass));

        out->hourly_count++;
    }

    if (!out->attribution[0]) {
        copy_attributions(root, out->attribution, sizeof(out->attribution));
    }
    if (out->hourly_count > 0) {
        out->valid_mask |= WEATHER_SNAPSHOT_HOURLY;
    }
    cJSON_Delete(root);
    return out->hourly_count > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static void parse_day_period(const cJSON *src,
                             char *text, size_t text_size,
                             char *code, size_t code_size,
                             float *humidity_pct,
                             float *precip_mm,
                             float *precip_probability_pct,
                             float *wind_speed_mps,
                             int *wind_scale)
{
    if (!src) {
        return;
    }
    copy_condition(src, text, text_size, code, code_size);

    if (humidity_pct) {
        *humidity_pct = num(src, "humidity", 0) * 100.0f;
    }

    const cJSON *precip = obj(src, "precipitation");
    if (precip_mm) {
        *precip_mm = nested_value(precip, "amount", 0);
    }
    if (precip_probability_pct) {
        *precip_probability_pct = num(precip, "probability", 0) * 100.0f;
    }

    const cJSON *wind = obj(src, "wind");
    if (wind_speed_mps) {
        *wind_speed_mps = nested_value(wind, "speed", 0);
    }
    if (wind_scale) {
        *wind_scale = (int)num(wind, "scale", 0);
    }
}

static esp_err_t parse_daily(const char *json, weather_snapshot_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *days = arr(root, "days");
    if (!days) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int count = cJSON_GetArraySize((cJSON *)days);
    if (count > (int)WEATHER_MAX_DAILY) {
        count = WEATHER_MAX_DAILY;
    }

    out->daily_count = 0;
    for (int i = 0; i < count; i++) {
        const cJSON *src = cJSON_GetArrayItem((cJSON *)days, i);
        if (!cJSON_IsObject(src)) {
            continue;
        }

        weather_daily_t *d = &out->daily[out->daily_count];
        memset(d, 0, sizeof(*d));

        copy_string(src, "forecastStartTime", d->forecast_start, sizeof(d->forecast_start));
        copy_string(src, "forecastEndTime", d->forecast_end, sizeof(d->forecast_end));
        d->temperature_max_c = nested_value(src, "temperatureMax", 0);
        d->temperature_min_c = nested_value(src, "temperatureMin", 0);
        d->temperature_avg_c = nested_value(src, "temperatureAvg", 0);
        d->uv_index_max = num(src, "uvIndexMax", 0);

        const cJSON *astro = obj(src, "astro");
        copy_string(astro, "sunrise", d->sunrise, sizeof(d->sunrise));
        copy_string(astro, "sunset", d->sunset, sizeof(d->sunset));
        copy_string(astro, "moonPhase", d->moon_phase, sizeof(d->moon_phase));

        parse_day_period(obj(src, "daytime"),
                         d->daytime_text, sizeof(d->daytime_text),
                         d->daytime_code, sizeof(d->daytime_code),
                         &d->daytime_humidity_pct,
                         &d->daytime_precip_mm,
                         &d->daytime_precip_probability_pct,
                         &d->daytime_wind_speed_mps,
                         &d->daytime_wind_scale);

        parse_day_period(obj(src, "nighttime"),
                         d->nighttime_text, sizeof(d->nighttime_text),
                         d->nighttime_code, sizeof(d->nighttime_code),
                         &d->nighttime_humidity_pct,
                         &d->nighttime_precip_mm,
                         &d->nighttime_precip_probability_pct,
                         &d->nighttime_wind_speed_mps,
                         &d->nighttime_wind_scale);

        out->daily_count++;
    }

    if (!out->attribution[0]) {
        copy_attributions(root, out->attribution, sizeof(out->attribution));
    }
    if (out->daily_count > 0) {
        out->valid_mask |= WEATHER_SNAPSHOT_DAILY;
    }
    cJSON_Delete(root);
    return out->daily_count > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t parse_alerts(const char *json, weather_snapshot_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    copy_attributions(root, out->alert_attribution, sizeof(out->alert_attribution));

    const cJSON *alerts = arr(root, "alerts");
    if (!alerts) {
        const cJSON *metadata = obj(root, "metadata");
        const cJSON *zero = metadata ? cJSON_GetObjectItemCaseSensitive((cJSON *)metadata, "zeroResult") : NULL;
        if (cJSON_IsTrue(zero)) {
            out->alert_count = 0;
            out->valid_mask |= WEATHER_SNAPSHOT_ALERTS;
            cJSON_Delete(root);
            return ESP_OK;
        }
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int count = cJSON_GetArraySize((cJSON *)alerts);
    if (count > (int)WEATHER_MAX_ALERTS) {
        count = WEATHER_MAX_ALERTS;
    }

    out->alert_count = 0;
    for (int i = 0; i < count; i++) {
        const cJSON *src = cJSON_GetArrayItem((cJSON *)alerts, i);
        if (!cJSON_IsObject(src)) {
            continue;
        }

        weather_alert_t *a = &out->alerts[out->alert_count];
        memset(a, 0, sizeof(*a));

        copy_string(src, "id", a->id, sizeof(a->id));
        copy_string(src, "senderName", a->sender, sizeof(a->sender));
        copy_string(src, "severity", a->severity, sizeof(a->severity));
        copy_string(src, "urgency", a->urgency, sizeof(a->urgency));
        copy_string(src, "certainty", a->certainty, sizeof(a->certainty));
        copy_string(src, "issuedTime", a->issued_time, sizeof(a->issued_time));
        copy_string(src, "effectiveTime", a->effective_time, sizeof(a->effective_time));
        copy_string(src, "onsetTime", a->onset_time, sizeof(a->onset_time));
        copy_string(src, "expireTime", a->expire_time, sizeof(a->expire_time));
        copy_string(src, "headline", a->headline, sizeof(a->headline));
        copy_string(src, "description", a->description, sizeof(a->description));
        copy_string(src, "instruction", a->instruction, sizeof(a->instruction));

        const cJSON *event = obj(src, "eventType");
        copy_string(event, "name", a->event_name, sizeof(a->event_name));
        copy_string(event, "code", a->event_code, sizeof(a->event_code));

        const cJSON *color = obj(src, "color");
        copy_string(color, "code", a->color, sizeof(a->color));

        out->alert_count++;
    }

    out->valid_mask |= WEATHER_SNAPSHOT_ALERTS;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t fetch_and_parse(const weather_qweather_config_t *cfg,
                                 const char *kind,
                                 esp_err_t (*parser)(const char *, weather_snapshot_t *),
                                 weather_snapshot_t *out)
{
    char url[QW_URL_MAX] = {0};
    esp_err_t err = build_url(url, sizeof(url), cfg, kind);
    if (err != ESP_OK) {
        return err;
    }

    char *json = NULL;
    err = qweather_http_get(url, cfg->api_key, &json);
    if (err != ESP_OK) {
        return err;
    }

    err = parser(json, out);
    free(json);
    return err;
}

static esp_err_t qweather_fetch(void *ctx, weather_snapshot_t *out)
{
    (void)ctx;
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    weather_qweather_config_t cfg = {0};
    esp_err_t err = weather_provider_qweather_get_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    if (!cfg.api_host[0] || !cfg.api_key[0] || !cfg.location_set) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out, 0, sizeof(*out));
    strlcpy(out->provider, "qweather", sizeof(out->provider));

    err = fetch_and_parse(&cfg, "current", parse_current, out);
    if (err != ESP_OK) {
        return err;
    }

    esp_err_t optional = fetch_and_parse(&cfg, "hourly", parse_hourly, out);
    if (optional != ESP_OK) {
        ESP_LOGW(TAG, "hourly fetch failed: %s", esp_err_to_name(optional));
    }

    optional = fetch_and_parse(&cfg, "daily", parse_daily, out);
    if (optional != ESP_OK) {
        ESP_LOGW(TAG, "daily fetch failed: %s", esp_err_to_name(optional));
    }

    optional = fetch_and_parse(&cfg, "alerts", parse_alerts, out);
    if (optional != ESP_OK) {
        ESP_LOGW(TAG, "alerts fetch failed: %s", esp_err_to_name(optional));
    }

    ESP_LOGI(TAG,
             "fetched current=%s hourly=%u daily=%u alerts=%u lat=%.2f lon=%.2f",
             out->current.valid ? "yes" : "no",
             (unsigned)out->hourly_count,
             (unsigned)out->daily_count,
             (unsigned)out->alert_count,
             cfg.latitude,
             cfg.longitude);
    return ESP_OK;
}

esp_err_t weather_provider_qweather_register(void)
{
    static const weather_provider_t provider = {
        .name = "qweather",
        .ctx = NULL,
        .is_configured = qweather_is_configured,
        .fetch = qweather_fetch,
    };
    return weather_service_register_provider(&provider);
}
