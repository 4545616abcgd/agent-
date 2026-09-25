/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "rtc_agent.h"
#include "sdkconfig.h"

static const char *TAG = "rtc_app";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_CONNECT_WAIT_MS 20000U

typedef struct {
    EventGroupHandle_t events;
    StaticEventGroup_t events_ctrl;
    uint32_t reconnects;
    bool started;
} app_wifi_ctx_t;

static app_wifi_ctx_t s_wifi;

static void time_sync_notification(struct timeval *tv)
{
    (void)tv;
    time_t now = 0;
    struct tm utc = {0};
    time(&now);
    gmtime_r(&now, &utc);
    ESP_LOGI(TAG, "SNTP synchronized: %04d-%02d-%02d %02d:%02d:%02d UTC",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec);
}

static void start_sntp(void)
{
    ESP_LOGI(TAG, "starting SNTP for hardware-agent request signing");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp1.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification);
    esp_sntp_init();
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "initial Wi-Fi connect failed: %s",
                     esp_err_to_name(err));
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        const unsigned reason = disconnected ? disconnected->reason : 0U;
        xEventGroupClearBits(s_wifi.events, WIFI_CONNECTED_BIT);
        if (!s_wifi.started) {
            return;
        }
        esp_err_t err = esp_wifi_connect();
        if (err == ESP_OK) {
            s_wifi.reconnects++;
            if ((s_wifi.reconnects % 10U) == 1U) {
                ESP_LOGW(TAG,
                         "Wi-Fi disconnected reason=%u; reconnect attempt=%u",
                         reason, (unsigned)s_wifi.reconnects);
            }
        } else {
            ESP_LOGE(TAG, "Wi-Fi reconnect request failed: %s",
                     esp_err_to_name(err));
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip = event_data;
        s_wifi.reconnects = 0;
        xEventGroupSetBits(s_wifi.events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected, address=" IPSTR,
                 IP2STR(&got_ip->ip_info.ip));
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG,
                 "NVS requires recovery (%s); refusing automatic erase to preserve retained settings",
                 esp_err_to_name(err));
    }
    return err;
}

static esp_err_t load_wifi_config(wifi_config_t *station_config,
                                  bool *loaded_from_nvs)
{
    ESP_RETURN_ON_FALSE(station_config && loaded_from_nvs,
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid Wi-Fi configuration destination");
    memset(station_config, 0, sizeof(*station_config));
    *loaded_from_nvs = false;

    if (CONFIG_RTC_AGENT_WIFI_SSID[0] != '\0') {
        size_t ssid_len = strlcpy((char *)station_config->sta.ssid,
                                  CONFIG_RTC_AGENT_WIFI_SSID,
                                  sizeof(station_config->sta.ssid));
        size_t password_len = strlcpy((char *)station_config->sta.password,
                                      CONFIG_RTC_AGENT_WIFI_PASSWORD,
                                      sizeof(station_config->sta.password));
        ESP_RETURN_ON_FALSE(ssid_len < sizeof(station_config->sta.ssid) &&
                                password_len < sizeof(station_config->sta.password),
                            ESP_ERR_INVALID_SIZE, TAG,
                            "Kconfig Wi-Fi credentials are too long");
        return ESP_OK;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("app", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_size = sizeof(station_config->sta.ssid);
    err = nvs_get_str(handle, "wifi_ssid",
                      (char *)station_config->sta.ssid, &ssid_size);
    if (err == ESP_OK) {
        size_t password_size = sizeof(station_config->sta.password);
        err = nvs_get_str(handle, "wifi_password",
                          (char *)station_config->sta.password,
                          &password_size);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            station_config->sta.password[0] = '\0';
            err = ESP_OK;
        }
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        return err;
    }
    ESP_RETURN_ON_FALSE(station_config->sta.ssid[0] != '\0',
                        ESP_ERR_NOT_FOUND, TAG,
                        "retained Wi-Fi SSID is empty");
    *loaded_from_nvs = true;
    return ESP_OK;
}

static esp_err_t start_wifi(void)
{
    wifi_config_t station_config = {0};
    bool loaded_from_nvs = false;
    esp_err_t config_err = load_wifi_config(&station_config,
                                            &loaded_from_nvs);
    if (config_err == ESP_ERR_NVS_NOT_FOUND ||
        config_err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG,
                 "Wi-Fi is not configured; local I2S/AFE/WakeNet will run offline");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(config_err, TAG,
                        "loading Wi-Fi credentials failed");
    if (loaded_from_nvs) {
        ESP_LOGI(TAG,
                 "using retained ESP-Claw Wi-Fi credentials from app NVS");
    }

    s_wifi.events = xEventGroupCreateStatic(&s_wifi.events_ctrl);
    ESP_RETURN_ON_FALSE(s_wifi.events, ESP_ERR_NO_MEM, TAG,
                        "Wi-Fi event group creation failed");

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(netif, ESP_ERR_NO_MEM, TAG,
                        "default Wi-Fi station creation failed");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG,
                        "Wi-Fi driver init failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   wifi_event_handler, NULL),
        TAG, "Wi-Fi event handler registration failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   wifi_event_handler, NULL),
        TAG, "IP event handler registration failed");

    station_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    station_config.sta.pmf_cfg.capable = true;
    station_config.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "Wi-Fi station mode setup failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &station_config), TAG,
                        "Wi-Fi credentials setup failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG,
                        "Wi-Fi latency mode setup failed");
    s_wifi.started = true;
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi.events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(WIFI_CONNECT_WAIT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG,
                 "Wi-Fi is still connecting; local voice startup will continue");
    }
    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_err_t wifi_err = start_wifi();
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi setup failed: %s; starting local voice offline",
                 esp_err_to_name(wifi_err));
    }

    start_sntp();

    ESP_ERROR_CHECK(rtc_agent_start());
}
