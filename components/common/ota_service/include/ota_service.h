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

#define OTA_SERVICE_URL_MAX        320
#define OTA_SERVICE_VERSION_MAX     32
#define OTA_SERVICE_CHANNEL_MAX     24
#define OTA_SERVICE_HARDWARE_MAX    48
#define OTA_SERVICE_NOTES_MAX      160
#define OTA_SERVICE_PARTITION_MAX   17

typedef enum {
    OTA_SERVICE_STATE_UNINITIALIZED = 0,
    OTA_SERVICE_STATE_IDLE,
    OTA_SERVICE_STATE_CHECKING,
    OTA_SERVICE_STATE_NO_UPDATE,
    OTA_SERVICE_STATE_AVAILABLE,
    OTA_SERVICE_STATE_DOWNLOADING,
    OTA_SERVICE_STATE_REBOOTING,
    OTA_SERVICE_STATE_ERROR,
} ota_service_state_t;

typedef struct {
    bool initialized;
    bool running;
    bool network_online;
    bool manifest_configured;
    bool auto_update;
    bool rollback_enabled;
    bool pending_verify;
    bool update_available;
    bool candidate_mandatory;

    ota_service_state_t state;
    esp_err_t last_error;
    int progress_pct;
    uint32_t check_interval_min;
    time_t last_check_at;
    time_t last_success_at;

    char current_version[OTA_SERVICE_VERSION_MAX];
    char current_partition[OTA_SERVICE_PARTITION_MAX];
    char next_partition[OTA_SERVICE_PARTITION_MAX];

    char manifest_url[OTA_SERVICE_URL_MAX];
    char channel[OTA_SERVICE_CHANNEL_MAX];
    char hardware_id[OTA_SERVICE_HARDWARE_MAX];

    char candidate_version[OTA_SERVICE_VERSION_MAX];
    char candidate_url[OTA_SERVICE_URL_MAX];
    char candidate_notes[OTA_SERVICE_NOTES_MAX];
} ota_service_status_t;

/**
 * Initialize the OTA service.
 *
 * app_config_init() must have already initialized the shared settings namespace.
 */
esp_err_t ota_service_init(void);

/** Start the background OTA worker. */
esp_err_t ota_service_start(void);

/** Update the service's view of Wi-Fi/internet availability. */
void ota_service_set_network_online(bool online);

/**
 * Signal that the application reached the end of its normal boot path.
 *
 * When bootloader rollback is enabled and this is the first boot of a newly
 * installed image, ota_service waits a validation grace period before marking
 * the image valid. A crash/reset before validation causes bootloader rollback.
 */
void ota_service_mark_boot_ready(void);

esp_err_t ota_service_get_status(ota_service_status_t *out_status);
const char *ota_service_state_name(ota_service_state_t state);

esp_err_t ota_service_set_manifest_url(const char *url);

/**
 * Configure native Bemfa OTA as the manifest provider.
 *
 * The generated request uses secure=true so Bemfa returns an HTTPS firmware URL.
 * open_id is sensitive and is stored only inside the existing ota_manifest NVS value.
 * device_type: 1=MQTT, 3=TCP, 5=MQTT v2, 7=TCP v2.
 */
esp_err_t ota_service_set_bemfa(const char *open_id,
                                const char *topic,
                                int device_type);

esp_err_t ota_service_set_channel(const char *channel);
esp_err_t ota_service_set_auto_update(bool enabled);
esp_err_t ota_service_set_check_interval_min(uint32_t minutes);

/** Queue an immediate manifest check. Non-blocking. */
esp_err_t ota_service_request_check(void);

/**
 * Queue an OTA installation. Non-blocking.
 *
 * If no current candidate is cached, the worker checks the manifest first.
 */
esp_err_t ota_service_request_update(void);

#ifdef __cplusplus
}
#endif
