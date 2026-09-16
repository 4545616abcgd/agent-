/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "ota_service.h"

static const char *TAG = "cmd_ota";

static void print_usage(void)
{
    printf(
        "OTA commands:\n"
        "  ota status\n"
        "  ota set manifest <https://host/path/manifest.json>\n"
        "  ota set bemfa <OPEN_ID> <TOPIC> [DEVICE_TYPE]\n"
        "  ota set channel <stable|beta|dev|...>\n"
        "  ota set auto <on|off>\n"
        "  ota set interval <minutes>    (5..10080)\n"
        "  ota check\n"
        "  ota update\n");
}

static int cmd_ota(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        ota_service_status_t s = {0};
        esp_err_t err = ota_service_get_status(&s);
        if (err != ESP_OK) {
            printf("OTA status failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        printf("OTA Service V1.2 (Bemfa-ready)\n");
        printf("  initialized : %s\n", s.initialized ? "yes" : "no");
        printf("  running     : %s\n", s.running ? "yes" : "no");
        printf("  network     : %s\n", s.network_online ? "online" : "offline");
        printf("  state       : %s\n", ota_service_state_name(s.state));
        printf("  current     : %s\n", s.current_version);
        printf("  partition   : %s -> %s\n",
               s.current_partition[0] ? s.current_partition : "(unknown)",
               s.next_partition[0] ? s.next_partition : "(unknown)");
        printf("  hardware    : %s\n", s.hardware_id);
        printf("  channel     : %s\n", s.channel);
        if (!s.manifest_configured) {
            printf("  manifest    : (not set)\n");
        } else if (strstr(s.manifest_url,
                          "apis.bemfa.com/vb/api/v1/firmwareVersion") != NULL) {
            printf("  provider    : bemfa\n");
            printf("  manifest    : Bemfa OTA API (credentials masked)\n");
        } else {
            const char *query = strchr(s.manifest_url, '?');
            if (query) {
                int prefix_len = (int)(query - s.manifest_url);
                printf("  manifest    : %.*s?...(query masked)\n",
                       prefix_len, s.manifest_url);
            } else {
                printf("  manifest    : %s\n", s.manifest_url);
            }
        }
        printf("  auto update : %s\n", s.auto_update ? "on" : "off");
        printf("  check every : %u min\n", (unsigned)s.check_interval_min);
        printf("  rollback    : %s pending_verify=%s\n",
               s.rollback_enabled ? "enabled" : "disabled",
               s.pending_verify ? "yes" : "no");
        printf("  last error  : %s\n", esp_err_to_name(s.last_error));
        printf("  progress    : %d %%\n", s.progress_pct);
        if (s.update_available) {
            printf("  candidate   : %s mandatory=%s\n",
                   s.candidate_version,
                   s.candidate_mandatory ? "yes" : "no");
            if (s.candidate_notes[0]) {
                printf("  notes       : %s\n", s.candidate_notes);
            }
        } else {
            printf("  candidate   : (none)\n");
        }
        return 0;
    }

    if (strcmp(argv[1], "check") == 0) {
        esp_err_t err = ota_service_request_check();
        if (err != ESP_OK) {
            printf("OTA check request failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("OTA provider check queued. Use `ota status` to inspect the result.\n");
        return 0;
    }

    if (strcmp(argv[1], "update") == 0) {
        esp_err_t err = ota_service_request_update();
        if (err != ESP_OK) {
            printf("OTA update request failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("OTA update queued. The device will reboot automatically after a successful install.\n");
        return 0;
    }

    if (strcmp(argv[1], "set") == 0 && argc >= 4) {
        esp_err_t err = ESP_ERR_INVALID_ARG;

        if (strcmp(argv[2], "manifest") == 0) {
            err = ota_service_set_manifest_url(argv[3]);
        } else if (strcmp(argv[2], "bemfa") == 0) {
            if (argc < 5) {
                printf("usage: ota set bemfa <OPEN_ID> <TOPIC> [DEVICE_TYPE]\n");
                return 1;
            }

            int device_type = 3;
            if (argc >= 6) {
                char *end = NULL;
                long value = strtol(argv[5], &end, 10);
                if (!end || *end != '\0' ||
                    (value != 1 && value != 3 && value != 5 && value != 7)) {
                    printf("DEVICE_TYPE must be 1, 3, 5, or 7\n");
                    return 1;
                }
                device_type = (int)value;
            }

            err = ota_service_set_bemfa(argv[3], argv[4], device_type);
        } else if (strcmp(argv[2], "channel") == 0) {
            err = ota_service_set_channel(argv[3]);
        } else if (strcmp(argv[2], "auto") == 0) {
            if (strcmp(argv[3], "on") == 0) {
                err = ota_service_set_auto_update(true);
            } else if (strcmp(argv[3], "off") == 0) {
                err = ota_service_set_auto_update(false);
            } else {
                printf("auto must be on or off\n");
                return 1;
            }
        } else if (strcmp(argv[2], "interval") == 0) {
            char *end = NULL;
            unsigned long minutes = strtoul(argv[3], &end, 10);
            if (!end || *end != '\0') {
                printf("interval must be an integer in minutes\n");
                return 1;
            }
            err = ota_service_set_check_interval_min((uint32_t)minutes);
        } else {
            print_usage();
            return 1;
        }

        if (err != ESP_OK) {
            printf("OTA setting failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        printf("OTA setting saved.\n");
        return 0;
    }

    print_usage();
    return 1;
}

esp_err_t register_ota_command(void)
{
    const esp_console_cmd_t command = {
        .command = "ota",
        .help = "Product HTTPS A/B OTA service",
        .hint = NULL,
        .func = &cmd_ota,
        .argtable = NULL,
    };

    esp_err_t err = esp_console_cmd_register(&command);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "registered: ota");
    }
    return err;
}
