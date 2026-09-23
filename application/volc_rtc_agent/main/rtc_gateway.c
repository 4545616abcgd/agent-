/* SPDX-License-Identifier: Apache-2.0 */

#include "rtc_gateway.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "sdkconfig.h"

static const char *TAG = "rtc_gateway";

#define GATEWAY_RESPONSE_BYTES 4096U
#define GATEWAY_URL_BYTES      320U

typedef struct {
    char body[GATEWAY_RESPONSE_BYTES];
    size_t used;
    bool overflow;
} response_buffer_t;

EXT_RAM_BSS_ATTR static response_buffer_t s_gateway_response;
EXT_RAM_BSS_ATTR static char s_gateway_url[GATEWAY_URL_BYTES];
EXT_RAM_BSS_ATTR static char s_gateway_authorization[320];

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_buffer_t *response = event->user_data;
    if (!response) {
        return ESP_OK;
    }
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        size_t available = sizeof(response->body) - 1U - response->used;
        size_t copy = (size_t)event->data_len;
        if (copy > available) {
            copy = available;
            response->overflow = true;
        }
        if (copy > 0) {
            memcpy(response->body + response->used, event->data, copy);
            response->used += copy;
            response->body[response->used] = '\0';
        }
    }
    return ESP_OK;
}

static size_t base_url_length(void)
{
    size_t len = strlen(CONFIG_RTC_AGENT_GATEWAY_URL);
    while (len > 0 && CONFIG_RTC_AGENT_GATEWAY_URL[len - 1U] == '/') {
        len--;
    }
    return len;
}

static esp_err_t gateway_post(const char *path,
                              const char *request_body,
                              response_buffer_t *response,
                              int *status_code)
{
    ESP_RETURN_ON_FALSE(path && response && status_code,
                        ESP_ERR_INVALID_ARG, TAG, "invalid HTTP arguments");
    const size_t root_len = base_url_length();
    int url_len = snprintf(s_gateway_url, sizeof(s_gateway_url), "%.*s%s",
                           (int)root_len,
                           CONFIG_RTC_AGENT_GATEWAY_URL, path);
    ESP_RETURN_ON_FALSE(url_len > 0 &&
                            (size_t)url_len < sizeof(s_gateway_url),
                        ESP_ERR_INVALID_SIZE, TAG, "gateway URL is too long");
    int auth_len = snprintf(s_gateway_authorization,
                            sizeof(s_gateway_authorization), "Bearer %s",
                            CONFIG_RTC_AGENT_DEVICE_API_KEY);
    ESP_RETURN_ON_FALSE(
        auth_len > 7 &&
            (size_t)auth_len < sizeof(s_gateway_authorization),
                        ESP_ERR_INVALID_SIZE, TAG, "device API key is too long");

    memset(response, 0, sizeof(*response));
    esp_http_client_config_t config = {
        .url = s_gateway_url,
        .event_handler = http_event,
        .user_data = response,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client, ESP_ERR_NO_MEM, TAG, "HTTP client init failed");

    esp_err_t err = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (err == ESP_OK) {
        err = esp_http_client_set_header(
            client, "Authorization", s_gateway_authorization);
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "Content-Type", "application/json");
    }
    if (err == ESP_OK && request_body) {
        err = esp_http_client_set_post_field(client, request_body,
                                             (int)strlen(request_body));
    }
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }
    if (err == ESP_OK) {
        *status_code = esp_http_client_get_status_code(client);
        if (response->overflow) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t copy_json_string(const cJSON *root,
                                  const char *name,
                                  char *destination,
                                  size_t capacity)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    const char *value = cJSON_GetStringValue(item);
    ESP_RETURN_ON_FALSE(value && value[0], ESP_ERR_INVALID_RESPONSE, TAG,
                        "gateway response has no %s", name);
    size_t len = strlen(value);
    ESP_RETURN_ON_FALSE(len < capacity, ESP_ERR_INVALID_SIZE, TAG,
                        "gateway field %s is too long", name);
    memcpy(destination, value, len + 1U);
    return ESP_OK;
}

bool rtc_gateway_is_configured(void)
{
    return CONFIG_RTC_AGENT_GATEWAY_URL[0] != '\0' &&
           CONFIG_RTC_AGENT_DEVICE_API_KEY[0] != '\0';
}

esp_err_t rtc_gateway_start_session(rtc_gateway_session_t *session)
{
    ESP_RETURN_ON_FALSE(session, ESP_ERR_INVALID_ARG, TAG, "session is null");
    memset(session, 0, sizeof(*session));
    ESP_RETURN_ON_FALSE(rtc_gateway_is_configured(), ESP_ERR_INVALID_STATE, TAG,
                        "RTC gateway URL/API key is not configured");

    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG,
                        "reading station MAC failed");
    char request[96];
    int request_len = snprintf(
        request, sizeof(request),
        "{\"device_id\":\"espclaw-%02x%02x%02x%02x%02x%02x\"}",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_RETURN_ON_FALSE(request_len > 0 && (size_t)request_len < sizeof(request),
                        ESP_ERR_INVALID_SIZE, TAG, "device id request overflow");

    response_buffer_t *response = &s_gateway_response;
    int status = 0;
    ESP_RETURN_ON_ERROR(
        gateway_post("/v1/rtc/sessions", request, response, &status),
        TAG, "gateway session request failed");
    ESP_RETURN_ON_FALSE(status == 201, ESP_ERR_INVALID_RESPONSE, TAG,
                        "gateway returned HTTP %d", status);

    cJSON *root = cJSON_ParseWithLength(response->body, response->used);
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_RESPONSE, TAG,
                        "gateway returned invalid JSON");
    esp_err_t err = copy_json_string(root, "session_id", session->session_id,
                                     sizeof(session->session_id));
    if (err == ESP_OK) {
        err = copy_json_string(root, "app_id", session->app_id,
                               sizeof(session->app_id));
    }
    if (err == ESP_OK) {
        err = copy_json_string(root, "room_id", session->room_id,
                               sizeof(session->room_id));
    }
    if (err == ESP_OK) {
        err = copy_json_string(root, "user_id", session->user_id,
                               sizeof(session->user_id));
    }
    if (err == ESP_OK) {
        err = copy_json_string(root, "token", session->token,
                               sizeof(session->token));
    }
    const cJSON *expires = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
    if (err == ESP_OK && !cJSON_IsNumber(expires)) {
        err = ESP_ERR_INVALID_RESPONSE;
    } else if (err == ESP_OK) {
        session->expires_at = (int64_t)expires->valuedouble;
    }
    cJSON_Delete(root);

    if (err != ESP_OK) {
        memset(session, 0, sizeof(*session));
        return err;
    }
    ESP_LOGI(TAG, "short-lived RTC session acquired (credentials not logged)");
    return ESP_OK;
}

esp_err_t rtc_gateway_stop_session(const rtc_gateway_session_t *session)
{
    if (!session || !session->session_id[0] || !rtc_gateway_is_configured()) {
        return ESP_OK;
    }
    char path[96];
    int len = snprintf(path, sizeof(path), "/v1/rtc/sessions/%s/stop",
                       session->session_id);
    ESP_RETURN_ON_FALSE(len > 0 && (size_t)len < sizeof(path),
                        ESP_ERR_INVALID_SIZE, TAG, "stop URL overflow");
    response_buffer_t *response = &s_gateway_response;
    int status = 0;
    ESP_RETURN_ON_ERROR(gateway_post(path, "{}", response, &status),
                        TAG, "gateway stop request failed");
    return status == 200 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
