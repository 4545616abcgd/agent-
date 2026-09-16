/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ota_service.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "settings_store.h"

#define OTA_KEY_MANIFEST   "ota_manifest"
#define OTA_KEY_CHANNEL    "ota_channel"
#define OTA_KEY_AUTO       "ota_auto"
#define OTA_KEY_INTERVAL   "ota_int_min"

#define OTA_NOTIFY_CHECK       (1U << 0)
#define OTA_NOTIFY_UPDATE      (1U << 1)
#define OTA_NOTIFY_BOOT_READY  (1U << 2)

static const char *TAG = "ota_service";

typedef struct {
    char version[OTA_SERVICE_VERSION_MAX];
    char url[OTA_SERVICE_URL_MAX];
    char hardware[OTA_SERVICE_HARDWARE_MAX];
    char channel[OTA_SERVICE_CHANNEL_MAX];
    char notes[OTA_SERVICE_NOTES_MAX];
    bool mandatory;
} ota_manifest_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool overflow;
} manifest_http_buffer_t;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t worker;
    ota_service_status_t status;
    bool boot_ready;
    TickType_t boot_ready_tick;
    TickType_t first_auto_tick;
    TickType_t next_auto_tick;
} ota_service_ctx_t;

static ota_service_ctx_t s_ota;

static void status_lock(void)
{
    if (s_ota.lock) {
        xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    }
}

static void status_unlock(void)
{
    if (s_ota.lock) {
        xSemaphoreGive(s_ota.lock);
    }
}

static bool ota_url_allowed(const char *url)
{
    if (!url || !url[0]) {
        return false;
    }
    if (strncmp(url, "https://", 8) == 0) {
        return true;
    }
#if CONFIG_OTA_SERVICE_ALLOW_HTTP
    if (strncmp(url, "http://", 7) == 0) {
        return true;
    }
#endif
    return false;
}

static bool system_time_valid(void)
{
    time_t now = time(NULL);
    struct tm tm_now = {0};
    if (now <= 0 || !gmtime_r(&now, &tm_now)) {
        return false;
    }
    return (tm_now.tm_year + 1900) >= 2024;
}

static void update_partition_labels_locked(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    s_ota.status.current_partition[0] = '\0';
    s_ota.status.next_partition[0] = '\0';

    if (running && running->label[0] != '\0') {
        strlcpy(s_ota.status.current_partition,
                running->label,
                sizeof(s_ota.status.current_partition));
    }
    if (next && next->label[0] != '\0') {
        strlcpy(s_ota.status.next_partition,
                next->label,
                sizeof(s_ota.status.next_partition));
    }
}

static void clear_candidate(void)
{
    status_lock();
    s_ota.status.update_available = false;
    s_ota.status.candidate_mandatory = false;
    s_ota.status.candidate_version[0] = '\0';
    s_ota.status.candidate_url[0] = '\0';
    s_ota.status.candidate_notes[0] = '\0';
    status_unlock();
}

static void set_state(ota_service_state_t state, esp_err_t err)
{
    status_lock();
    s_ota.status.state = state;
    s_ota.status.last_error = err;
    if (state != OTA_SERVICE_STATE_DOWNLOADING) {
        s_ota.status.progress_pct = 0;
    }
    status_unlock();
}

static void set_progress(int pct)
{
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }

    status_lock();
    s_ota.status.progress_pct = pct;
    status_unlock();
}

static int parse_semver_triplet(const char *s, int out[3])
{
    const char *p = s;
    for (int i = 0; i < 3; i++) {
        if (!p || !isdigit((unsigned char)*p)) {
            return -1;
        }

        long value = 0;
        while (isdigit((unsigned char)*p)) {
            value = value * 10 + (*p - '0');
            if (value > 1000000L) {
                return -1;
            }
            p++;
        }
        out[i] = (int)value;

        if (i < 2) {
            if (*p != '.') {
                return -1;
            }
            p++;
        }
    }

    if (*p != '\0' && *p != '-' && *p != '+') {
        return -1;
    }
    return 0;
}

static int compare_versions(const char *a, const char *b)
{
    int av[3] = {0};
    int bv[3] = {0};

    if (parse_semver_triplet(a, av) != 0 || parse_semver_triplet(b, bv) != 0) {
        return 0;
    }

    for (int i = 0; i < 3; i++) {
        if (av[i] < bv[i]) {
            return -1;
        }
        if (av[i] > bv[i]) {
            return 1;
        }
    }
    return 0;
}

static esp_err_t load_runtime_config(void)
{
    char auto_buf[8] = {0};
    char interval_buf[16] = {0};

    status_lock();
    esp_err_t err = settings_store_get_string(OTA_KEY_MANIFEST,
                                               s_ota.status.manifest_url,
                                               sizeof(s_ota.status.manifest_url),
                                               CONFIG_OTA_SERVICE_DEFAULT_MANIFEST_URL);
    if (err == ESP_OK) {
        err = settings_store_get_string(OTA_KEY_CHANNEL,
                                        s_ota.status.channel,
                                        sizeof(s_ota.status.channel),
                                        CONFIG_OTA_SERVICE_DEFAULT_CHANNEL);
    }
    if (err == ESP_OK) {
        err = settings_store_get_string(OTA_KEY_AUTO,
                                        auto_buf,
                                        sizeof(auto_buf),
#if CONFIG_OTA_SERVICE_DEFAULT_AUTO_UPDATE
                                        "1"
#else
                                        "0"
#endif
        );
    }
    if (err == ESP_OK) {
        char default_interval[16];
        snprintf(default_interval, sizeof(default_interval), "%d",
                 CONFIG_OTA_SERVICE_DEFAULT_CHECK_INTERVAL_MIN);
        err = settings_store_get_string(OTA_KEY_INTERVAL,
                                        interval_buf,
                                        sizeof(interval_buf),
                                        default_interval);
    }

    if (err == ESP_OK) {
        s_ota.status.auto_update = strcmp(auto_buf, "1") == 0 ||
                                   strcasecmp(auto_buf, "true") == 0 ||
                                   strcasecmp(auto_buf, "on") == 0;
        unsigned long value = strtoul(interval_buf, NULL, 10);
        if (value < 5 || value > 10080) {
            value = CONFIG_OTA_SERVICE_DEFAULT_CHECK_INTERVAL_MIN;
        }
        s_ota.status.check_interval_min = (uint32_t)value;
        s_ota.status.manifest_configured = s_ota.status.manifest_url[0] != '\0';
        strlcpy(s_ota.status.hardware_id,
                CONFIG_OTA_SERVICE_HARDWARE_ID,
                sizeof(s_ota.status.hardware_id));
    }
    status_unlock();

    return err;
}

static esp_err_t manifest_http_event(esp_http_client_event_t *evt)
{
    manifest_http_buffer_t *buf = (manifest_http_buffer_t *)evt->user_data;
    if (!buf) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        size_t incoming = (size_t)evt->data_len;
        if (buf->len + incoming >= buf->cap) {
            buf->overflow = true;
            return ESP_OK;
        }
        memcpy(buf->data + buf->len, evt->data, incoming);
        buf->len += incoming;
        buf->data[buf->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t fetch_manifest_json(const char *url, char **out_json)
{
    if (!url || !out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;

    size_t cap = (size_t)CONFIG_OTA_SERVICE_MANIFEST_MAX_BYTES + 1U;
    char *storage = calloc(1, cap);
    if (!storage) {
        return ESP_ERR_NO_MEM;
    }

    manifest_http_buffer_t response = {
        .data = storage,
        .cap = cap,
    };

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = CONFIG_OTA_SERVICE_HTTP_TIMEOUT_MS,
        .event_handler = manifest_http_event,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(storage);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Cache-Control", "no-cache");
    esp_http_client_set_header(client, "User-Agent", "ESP-Claw-OTA/1");

    esp_err_t err = esp_http_client_perform(client);
    int http_status = esp_http_client_get_status_code(client);

    if (err == ESP_OK && response.overflow) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && http_status != 200) {
        ESP_LOGW(TAG, "manifest HTTP status=%d", http_status);
        err = ESP_FAIL;
    }
    if (err == ESP_OK && response.len == 0) {
        err = ESP_ERR_INVALID_RESPONSE;
    }

    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        free(storage);
        return err;
    }

    *out_json = storage;
    return ESP_OK;
}

static esp_err_t parse_manifest(const char *json_text, ota_manifest_t *out)
{
    if (!json_text || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json_text);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /*
     * Bemfa native OTA API response:
     * {
     *   "code": 0,
     *   "data": {
     *     "url": "https://...bin",
     *     "version": 2,
     *     "tag": "0.1.1",
     *     "size": 2850000
     *   }
     * }
     *
     * We deliberately use Bemfa `tag` as the semantic application version.
     * Therefore the release tag MUST exactly match esp_app_desc_t.version,
     * for example 0.1.1. This preserves the same strict binary/version check
     * used by the original manifest provider.
     */
    cJSON *bemfa_code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (bemfa_code) {
        cJSON *bemfa_data = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (!cJSON_IsNumber(bemfa_code) || bemfa_code->valueint != 0) {
            cJSON_Delete(root);
            return ESP_FAIL;
        }
        if (!cJSON_IsObject(bemfa_data)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }

        cJSON *bemfa_url = cJSON_GetObjectItemCaseSensitive(bemfa_data, "url");
        cJSON *bemfa_tag = cJSON_GetObjectItemCaseSensitive(bemfa_data, "tag");
        cJSON *bemfa_revision = cJSON_GetObjectItemCaseSensitive(bemfa_data, "version");
        cJSON *bemfa_size = cJSON_GetObjectItemCaseSensitive(bemfa_data, "size");

        if (!cJSON_IsString(bemfa_url) || !bemfa_url->valuestring ||
            !cJSON_IsString(bemfa_tag) || !bemfa_tag->valuestring) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (parse_semver_triplet(bemfa_tag->valuestring, (int[3]){0}) != 0) {
            ESP_LOGE(TAG, "Bemfa firmware tag must be semantic version, got: %s",
                     bemfa_tag->valuestring);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_VERSION;
        }
        if (!ota_url_allowed(bemfa_url->valuestring)) {
            ESP_LOGE(TAG, "Bemfa firmware URL is not allowed; request secure=true");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }

        strlcpy(out->version, bemfa_tag->valuestring, sizeof(out->version));
        strlcpy(out->url, bemfa_url->valuestring, sizeof(out->url));

        int revision = cJSON_IsNumber(bemfa_revision) ? bemfa_revision->valueint : 0;
        int size = cJSON_IsNumber(bemfa_size) ? bemfa_size->valueint : 0;
        snprintf(out->notes, sizeof(out->notes),
                 "Bemfa OTA revision=%d size=%d", revision, size);

        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
    cJSON *hardware = cJSON_GetObjectItemCaseSensitive(root, "hardware");
    cJSON *channel = cJSON_GetObjectItemCaseSensitive(root, "channel");
    cJSON *mandatory = cJSON_GetObjectItemCaseSensitive(root, "mandatory");
    cJSON *notes = cJSON_GetObjectItemCaseSensitive(root, "notes");

    if (schema && (!cJSON_IsNumber(schema) || schema->valueint != 1)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_VERSION;
    }

    if (!cJSON_IsString(version) || !version->valuestring ||
        !cJSON_IsString(url) || !url->valuestring) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (parse_semver_triplet(version->valuestring, (int[3]){0}) != 0) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_VERSION;
    }
    if (!ota_url_allowed(url->valuestring)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(out->version, version->valuestring, sizeof(out->version));
    strlcpy(out->url, url->valuestring, sizeof(out->url));

    if (cJSON_IsString(hardware) && hardware->valuestring) {
        strlcpy(out->hardware, hardware->valuestring, sizeof(out->hardware));
    }
    if (cJSON_IsString(channel) && channel->valuestring) {
        strlcpy(out->channel, channel->valuestring, sizeof(out->channel));
    }
    if (cJSON_IsString(notes) && notes->valuestring) {
        strlcpy(out->notes, notes->valuestring, sizeof(out->notes));
    }
    if (cJSON_IsBool(mandatory)) {
        out->mandatory = cJSON_IsTrue(mandatory);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t check_manifest(bool from_user, bool *available_out)
{
    if (available_out) {
        *available_out = false;
    }

    char manifest_url[OTA_SERVICE_URL_MAX] = {0};
    char channel[OTA_SERVICE_CHANNEL_MAX] = {0};
    char hardware[OTA_SERVICE_HARDWARE_MAX] = {0};
    char current_version[OTA_SERVICE_VERSION_MAX] = {0};
    bool online = false;

    status_lock();
    strlcpy(manifest_url, s_ota.status.manifest_url, sizeof(manifest_url));
    strlcpy(channel, s_ota.status.channel, sizeof(channel));
    strlcpy(hardware, s_ota.status.hardware_id, sizeof(hardware));
    strlcpy(current_version, s_ota.status.current_version, sizeof(current_version));
    online = s_ota.status.network_online;
    s_ota.status.last_check_at = time(NULL);
    status_unlock();

    if (!manifest_url[0]) {
        if (from_user) {
            ESP_LOGW(TAG, "manifest URL is not configured");
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (!online) {
        if (from_user) {
            ESP_LOGW(TAG, "network is offline");
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (!system_time_valid()) {
        if (from_user) {
            ESP_LOGW(TAG, "system time is not synchronized yet");
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (!ota_url_allowed(manifest_url)) {
        return ESP_ERR_INVALID_ARG;
    }

    clear_candidate();
    set_state(OTA_SERVICE_STATE_CHECKING, ESP_OK);

    char *json_text = NULL;
    esp_err_t err = fetch_manifest_json(manifest_url, &json_text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "manifest fetch failed: %s", esp_err_to_name(err));
        set_state(OTA_SERVICE_STATE_ERROR, err);
        return err;
    }

    ota_manifest_t manifest = {0};
    err = parse_manifest(json_text, &manifest);
    free(json_text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "manifest parse failed: %s", esp_err_to_name(err));
        set_state(OTA_SERVICE_STATE_ERROR, err);
        return err;
    }

    if (manifest.hardware[0] &&
        strcmp(manifest.hardware, "*") != 0 &&
        strcmp(manifest.hardware, hardware) != 0) {
        ESP_LOGW(TAG, "manifest hardware mismatch: device=%s manifest=%s",
                 hardware, manifest.hardware);
        set_state(OTA_SERVICE_STATE_NO_UPDATE, ESP_OK);
        return ESP_OK;
    }

    if (manifest.channel[0] && strcmp(manifest.channel, channel) != 0) {
        ESP_LOGI(TAG, "manifest channel mismatch: device=%s manifest=%s",
                 channel, manifest.channel);
        set_state(OTA_SERVICE_STATE_NO_UPDATE, ESP_OK);
        return ESP_OK;
    }

    int cmp = compare_versions(manifest.version, current_version);
    if (cmp <= 0) {
        ESP_LOGI(TAG, "no newer release: current=%s manifest=%s",
                 current_version, manifest.version);
        set_state(OTA_SERVICE_STATE_NO_UPDATE, ESP_OK);
        return ESP_OK;
    }

    status_lock();
    s_ota.status.update_available = true;
    s_ota.status.candidate_mandatory = manifest.mandatory;
    strlcpy(s_ota.status.candidate_version,
            manifest.version,
            sizeof(s_ota.status.candidate_version));
    strlcpy(s_ota.status.candidate_url,
            manifest.url,
            sizeof(s_ota.status.candidate_url));
    strlcpy(s_ota.status.candidate_notes,
            manifest.notes,
            sizeof(s_ota.status.candidate_notes));
    status_unlock();

    ESP_LOGI(TAG, "new release available: %s -> %s mandatory=%d",
             current_version, manifest.version, manifest.mandatory);
    set_state(OTA_SERVICE_STATE_AVAILABLE, ESP_OK);

    if (available_out) {
        *available_out = true;
    }
    return ESP_OK;
}

static esp_err_t validate_downloaded_header(const esp_app_desc_t *new_app,
                                            const ota_manifest_t *candidate)
{
    if (!new_app || !candidate) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    if (!running) {
        return ESP_FAIL;
    }

    if (strcmp(new_app->project_name, running->project_name) != 0) {
        ESP_LOGE(TAG, "project mismatch: running=%s update=%s",
                 running->project_name, new_app->project_name);
        return ESP_ERR_INVALID_VERSION;
    }

    if (strcmp(new_app->version, candidate->version) != 0) {
        ESP_LOGE(TAG, "binary/manifest version mismatch: manifest=%s binary=%s",
                 candidate->version, new_app->version);
        return ESP_ERR_INVALID_VERSION;
    }

    if (compare_versions(new_app->version, running->version) <= 0) {
        ESP_LOGW(TAG, "refusing non-upgrade image: running=%s update=%s",
                 running->version, new_app->version);
        return ESP_ERR_INVALID_VERSION;
    }

    return ESP_OK;
}

static esp_err_t install_candidate(void)
{
    ota_manifest_t candidate = {0};
    bool online = false;

    status_lock();
    online = s_ota.status.network_online;
    strlcpy(candidate.version,
            s_ota.status.candidate_version,
            sizeof(candidate.version));
    strlcpy(candidate.url,
            s_ota.status.candidate_url,
            sizeof(candidate.url));
    strlcpy(candidate.notes,
            s_ota.status.candidate_notes,
            sizeof(candidate.notes));
    candidate.mandatory = s_ota.status.candidate_mandatory;
    status_unlock();

    if (!online || !candidate.version[0] || !candidate.url[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!system_time_valid()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ota_url_allowed(candidate.url)) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "starting OTA download version=%s", candidate.version);
    set_state(OTA_SERVICE_STATE_DOWNLOADING, ESP_OK);
    set_progress(0);

    esp_http_client_config_t http_cfg = {
        .url = candidate.url,
        .timeout_ms = CONFIG_OTA_SERVICE_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
        .bulk_flash_erase = false,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        set_state(OTA_SERVICE_STATE_ERROR, err);
        return err;
    }

    esp_app_desc_t new_app = {0};
    err = esp_https_ota_get_img_desc(handle, &new_app);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to read OTA image descriptor: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        set_state(OTA_SERVICE_STATE_ERROR, err);
        return err;
    }

    err = validate_downloaded_header(&new_app, &candidate);
    if (err != ESP_OK) {
        esp_https_ota_abort(handle);
        set_state(OTA_SERVICE_STATE_ERROR, err);
        return err;
    }

    int total = esp_https_ota_get_image_size(handle);
    int last_logged_pct = -10;

    while (true) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        int done = esp_https_ota_get_image_len_read(handle);
        if (done >= 0 && total > 0) {
            int pct = (int)(((int64_t)done * 100) / total);
            set_progress(pct);
            if (pct >= last_logged_pct + 10) {
                ESP_LOGI(TAG, "OTA progress %d%% (%d/%d)", pct, done, total);
                last_logged_pct = pct;
            }
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        set_state(OTA_SERVICE_STATE_ERROR, err);
        return err;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "OTA connection closed before complete image was received");
        esp_https_ota_abort(handle);
        set_state(OTA_SERVICE_STATE_ERROR, ESP_FAIL);
        return ESP_FAIL;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish/validation failed: %s", esp_err_to_name(err));
        set_state(OTA_SERVICE_STATE_ERROR, err);
        return err;
    }

    status_lock();
    s_ota.status.progress_pct = 100;
    s_ota.status.last_success_at = time(NULL);
    s_ota.status.state = OTA_SERVICE_STATE_REBOOTING;
    s_ota.status.last_error = ESP_OK;
    update_partition_labels_locked();
    status_unlock();

    ESP_LOGI(TAG, "OTA upgrade installed successfully; rebooting into %s",
             s_ota.status.next_partition[0] ? s_ota.status.next_partition : "new OTA slot");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

static void confirm_running_image_if_due(void)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    bool should_confirm = false;

    status_lock();
    if (s_ota.status.pending_verify && s_ota.boot_ready) {
        TickType_t elapsed = xTaskGetTickCount() - s_ota.boot_ready_tick;
        should_confirm = elapsed >= pdMS_TO_TICKS(CONFIG_OTA_SERVICE_VALIDATE_DELAY_SEC * 1000);
    }
    status_unlock();

    if (!should_confirm) {
        return;
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        status_lock();
        s_ota.status.pending_verify = false;
        status_unlock();
        ESP_LOGI(TAG, "new firmware marked VALID after %d s health grace period",
                 CONFIG_OTA_SERVICE_VALIDATE_DELAY_SEC);
    } else {
        ESP_LOGE(TAG, "failed to mark running image valid: %s", esp_err_to_name(err));
    }
#endif
}

static bool ota_busy(void)
{
    bool busy;
    status_lock();
    busy = s_ota.status.state == OTA_SERVICE_STATE_CHECKING ||
           s_ota.status.state == OTA_SERVICE_STATE_DOWNLOADING ||
           s_ota.status.state == OTA_SERVICE_STATE_REBOOTING;
    status_unlock();
    return busy;
}

static void ota_worker_task(void *arg)
{
    (void)arg;

    TickType_t now = xTaskGetTickCount();
    status_lock();
    s_ota.status.running = true;
    s_ota.first_auto_tick = now + pdMS_TO_TICKS(CONFIG_OTA_SERVICE_INITIAL_CHECK_DELAY_SEC * 1000);
    s_ota.next_auto_tick = s_ota.first_auto_tick;
    status_unlock();

    while (true) {
        uint32_t notify = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notify, pdMS_TO_TICKS(1000));

        if (notify & OTA_NOTIFY_BOOT_READY) {
            status_lock();
            s_ota.boot_ready = true;
            s_ota.boot_ready_tick = xTaskGetTickCount();
            status_unlock();
        }

        confirm_running_image_if_due();

        if (notify & OTA_NOTIFY_CHECK) {
            bool available = false;
            (void)check_manifest(true, &available);
        }

        if (notify & OTA_NOTIFY_UPDATE) {
            bool available = false;
            esp_err_t check_err = check_manifest(true, &available);
            if (check_err != ESP_OK) {
                continue;
            }

            if (available) {
                (void)install_candidate();
            }
        }

        now = xTaskGetTickCount();

        bool auto_due = false;
        bool configured = false;
        bool online = false;
        bool auto_update = false;
        bool pending_verify = false;
        uint32_t interval_min = 0;

        status_lock();
        configured = s_ota.status.manifest_configured;
        online = s_ota.status.network_online;
        auto_update = s_ota.status.auto_update;
        pending_verify = s_ota.status.pending_verify;
        interval_min = s_ota.status.check_interval_min;
        auto_due = (int32_t)(now - s_ota.next_auto_tick) >= 0;
        status_unlock();

        if (!ota_busy() &&
            !pending_verify &&
            configured &&
            online &&
            system_time_valid() &&
            auto_due) {
            bool available = false;
            esp_err_t err = check_manifest(false, &available);

            status_lock();
            s_ota.next_auto_tick = xTaskGetTickCount() +
                                   pdMS_TO_TICKS(interval_min * 60U * 1000U);
            status_unlock();

            if (err == ESP_OK && available && auto_update) {
                (void)install_candidate();
            }
        }
    }
}

esp_err_t ota_service_init(void)
{
    if (s_ota.status.initialized) {
        return ESP_OK;
    }

    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.lock = xSemaphoreCreateMutex();
    if (!s_ota.lock) {
        return ESP_ERR_NO_MEM;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    status_lock();
    s_ota.status.initialized = true;
    s_ota.status.state = OTA_SERVICE_STATE_IDLE;
    s_ota.status.last_error = ESP_OK;
    if (app) {
        strlcpy(s_ota.status.current_version,
                app->version,
                sizeof(s_ota.status.current_version));
    }
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    s_ota.status.rollback_enabled = true;
#else
    s_ota.status.rollback_enabled = false;
#endif
    update_partition_labels_locked();
    status_unlock();

    esp_err_t err = load_runtime_config();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to load OTA settings: %s", esp_err_to_name(err));
        return err;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &image_state) == ESP_OK) {
        status_lock();
        s_ota.status.pending_verify = (image_state == ESP_OTA_IMG_PENDING_VERIFY);
        status_unlock();
    }

    ESP_LOGI(TAG,
             "initialized current=%s partition=%s next=%s manifest=%s auto_update=%d interval=%u min rollback=%d pending_verify=%d",
             s_ota.status.current_version,
             s_ota.status.current_partition,
             s_ota.status.next_partition,
             s_ota.status.manifest_configured ? "configured" : "unset",
             s_ota.status.auto_update,
             (unsigned)s_ota.status.check_interval_min,
             s_ota.status.rollback_enabled,
             s_ota.status.pending_verify);

#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    ESP_LOGW(TAG,
             "bootloader app rollback is disabled; enable CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE before production");
#endif

    return ESP_OK;
}

esp_err_t ota_service_start(void)
{
    if (!s_ota.status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ota.worker) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(ota_worker_task,
                                "ota_service",
                                CONFIG_OTA_SERVICE_TASK_STACK_SIZE,
                                NULL,
                                4,
                                &s_ota.worker);
    if (ok != pdPASS) {
        s_ota.worker = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ota_service_set_network_online(bool online)
{
    if (!s_ota.status.initialized) {
        return;
    }
    status_lock();
    s_ota.status.network_online = online;
    status_unlock();
}

void ota_service_mark_boot_ready(void)
{
    if (!s_ota.worker) {
        return;
    }
    xTaskNotify(s_ota.worker, OTA_NOTIFY_BOOT_READY, eSetBits);
}

esp_err_t ota_service_get_status(ota_service_status_t *out_status)
{
    if (!out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ota.status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    status_lock();
    memcpy(out_status, &s_ota.status, sizeof(*out_status));
    status_unlock();
    return ESP_OK;
}

const char *ota_service_state_name(ota_service_state_t state)
{
    switch (state) {
    case OTA_SERVICE_STATE_UNINITIALIZED:
        return "uninitialized";
    case OTA_SERVICE_STATE_IDLE:
        return "idle";
    case OTA_SERVICE_STATE_CHECKING:
        return "checking";
    case OTA_SERVICE_STATE_NO_UPDATE:
        return "no_update";
    case OTA_SERVICE_STATE_AVAILABLE:
        return "available";
    case OTA_SERVICE_STATE_DOWNLOADING:
        return "downloading";
    case OTA_SERVICE_STATE_REBOOTING:
        return "rebooting";
    case OTA_SERVICE_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

esp_err_t ota_service_set_manifest_url(const char *url)
{
    if (!s_ota.status.initialized || !url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(url) >= OTA_SERVICE_URL_MAX || !ota_url_allowed(url)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ota_busy()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = settings_store_set_string(OTA_KEY_MANIFEST, url);
    if (err != ESP_OK) {
        return err;
    }

    status_lock();
    strlcpy(s_ota.status.manifest_url, url, sizeof(s_ota.status.manifest_url));
    s_ota.status.manifest_configured = true;
    s_ota.status.update_available = false;
    s_ota.status.candidate_version[0] = '\0';
    s_ota.status.candidate_url[0] = '\0';
    s_ota.next_auto_tick = xTaskGetTickCount();
    status_unlock();
    return ESP_OK;
}

static bool ota_bemfa_token_valid(const char *value)
{
    if (!value || !value[0]) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (!isalnum(*p)) {
            return false;
        }
    }
    return true;
}

esp_err_t ota_service_set_bemfa(const char *open_id,
                                const char *topic,
                                int device_type)
{
    if (!s_ota.status.initialized || !open_id || !topic) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ota_bemfa_token_valid(open_id) || !ota_bemfa_token_valid(topic)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (device_type != 1 && device_type != 3 &&
        device_type != 5 && device_type != 7) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[OTA_SERVICE_URL_MAX];
    int n = snprintf(url, sizeof(url),
                     "https://apis.bemfa.com/vb/api/v1/firmwareVersion"
                     "?openID=%s&topic=%s&deviceType=%d&secure=true",
                     open_id, topic, device_type);
    if (n <= 0 || n >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ota_service_set_manifest_url(url);
}

esp_err_t ota_service_set_channel(const char *channel)
{
    if (!s_ota.status.initialized || !channel || !channel[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(channel) >= OTA_SERVICE_CHANNEL_MAX || ota_busy()) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = settings_store_set_string(OTA_KEY_CHANNEL, channel);
    if (err != ESP_OK) {
        return err;
    }

    status_lock();
    strlcpy(s_ota.status.channel, channel, sizeof(s_ota.status.channel));
    s_ota.status.update_available = false;
    s_ota.status.candidate_version[0] = '\0';
    s_ota.status.candidate_url[0] = '\0';
    s_ota.next_auto_tick = xTaskGetTickCount();
    status_unlock();
    return ESP_OK;
}

esp_err_t ota_service_set_auto_update(bool enabled)
{
    if (!s_ota.status.initialized || ota_busy()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = settings_store_set_string(OTA_KEY_AUTO, enabled ? "1" : "0");
    if (err != ESP_OK) {
        return err;
    }

    status_lock();
    s_ota.status.auto_update = enabled;
    status_unlock();
    return ESP_OK;
}

esp_err_t ota_service_set_check_interval_min(uint32_t minutes)
{
    if (!s_ota.status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (minutes < 5 || minutes > 10080 || ota_busy()) {
        return ESP_ERR_INVALID_ARG;
    }

    char value[16];
    snprintf(value, sizeof(value), "%u", (unsigned)minutes);
    esp_err_t err = settings_store_set_string(OTA_KEY_INTERVAL, value);
    if (err != ESP_OK) {
        return err;
    }

    status_lock();
    s_ota.status.check_interval_min = minutes;
    s_ota.next_auto_tick = xTaskGetTickCount() + pdMS_TO_TICKS(minutes * 60U * 1000U);
    status_unlock();
    return ESP_OK;
}

esp_err_t ota_service_request_check(void)
{
    if (!s_ota.worker) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ota_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotify(s_ota.worker, OTA_NOTIFY_CHECK, eSetBits);
    return ESP_OK;
}

esp_err_t ota_service_request_update(void)
{
    if (!s_ota.worker) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ota_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotify(s_ota.worker, OTA_NOTIFY_UPDATE, eSetBits);
    return ESP_OK;
}
