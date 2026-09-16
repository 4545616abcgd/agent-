/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "weather_provider_qweather.h"
#include "weather_service.h"

static const char *TAG = "cmd_weather";

static void print_usage(void)
{
    printf("Usage:\n");
    printf("  weather status\n");
    printf("  weather config\n");
    printf("  weather set host <your-api-host.qweatherapi.com>\n");
    printf("  weather set key <API_KEY>\n");
    printf("  weather set location <latitude> <longitude>\n");
    printf("  weather clear-key\n");
    printf("  weather refresh\n");
    printf("  weather current\n");
    printf("  weather hourly [count]\n");
    printf("  weather daily [count]\n");
    printf("  weather alerts\n");
}

static void print_masked_key(const char *key)
{
    size_t n = key ? strlen(key) : 0;
    if (n == 0) {
        printf("(not set)");
    } else if (n <= 8) {
        printf("********");
    } else {
        printf("%.4s...%s", key, key + n - 4);
    }
}

static int parse_count(int argc, char **argv, int default_count, int max_count)
{
    if (argc < 3) {
        return default_count;
    }
    char *end = NULL;
    long v = strtol(argv[2], &end, 10);
    if (!end || *end != '\0' || v < 1 || v > max_count) {
        return -1;
    }
    return (int)v;
}

static int cmd_weather(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        weather_service_status_t st = {0};
        weather_service_get_status(&st);
        printf("Weather Service V1\n");
        printf("  initialized : %s\n", st.initialized ? "yes" : "no");
        printf("  running     : %s\n", st.running ? "yes" : "no");
        printf("  network     : %s\n", st.network_online ? "online" : "offline");
        printf("  provider    : %s\n", st.provider[0] ? st.provider : "(none)");
        printf("  configured  : %s\n", st.provider_configured ? "yes" : "no");
        printf("  refresh     : %u min\n", (unsigned)(st.refresh_interval_ms / 60000U));
        printf("  last error  : %s\n", esp_err_to_name(st.last_error));
        printf("  last attempt: %lld\n", (long long)st.last_attempt_at);
        printf("  last success: %lld\n", (long long)st.last_success_at);
        return 0;
    }

    if (strcmp(argv[1], "config") == 0) {
        weather_qweather_config_t cfg = {0};
        esp_err_t err = weather_provider_qweather_get_config(&cfg);
        if (err != ESP_OK) {
            printf("weather config failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        printf("QWeather config\n");
        printf("  host     : %s\n", cfg.api_host[0] ? cfg.api_host : "(not set)");
        printf("  API key  : ");
        print_masked_key(cfg.api_key);
        printf("\n");
        if (cfg.location_set) {
            printf("  location : %.4f, %.4f (lat, lon)\n", cfg.latitude, cfg.longitude);
        } else {
            printf("  location : (not set)\n");
        }
        return 0;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 4) {
            print_usage();
            return 1;
        }

        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (strcmp(argv[2], "host") == 0 && argc == 4) {
            err = weather_provider_qweather_set_host(argv[3]);
            if (err == ESP_OK) {
                printf("QWeather API Host saved.\n");
            }
        } else if (strcmp(argv[2], "key") == 0 && argc == 4) {
            err = weather_provider_qweather_set_api_key(argv[3]);
            if (err == ESP_OK) {
                printf("QWeather API key saved (value hidden).\n");
            }
        } else if (strcmp(argv[2], "location") == 0 && argc == 5) {
            char *end_lat = NULL;
            char *end_lon = NULL;
            double lat = strtod(argv[3], &end_lat);
            double lon = strtod(argv[4], &end_lon);
            if (!end_lat || *end_lat != '\0' || !end_lon || *end_lon != '\0') {
                err = ESP_ERR_INVALID_ARG;
            } else {
                err = weather_provider_qweather_set_location(lat, lon);
            }
            if (err == ESP_OK) {
                printf("Weather location saved: %.4f, %.4f (lat, lon)\n", lat, lon);
            }
        }

        if (err != ESP_OK) {
            printf("weather set failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        (void)weather_service_request_refresh();
        return 0;
    }

    if (strcmp(argv[1], "clear-key") == 0) {
        esp_err_t err = weather_provider_qweather_clear_api_key();
        if (err != ESP_OK) {
            printf("weather clear-key failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("QWeather API key removed.\n");
        return 0;
    }

    if (strcmp(argv[1], "refresh") == 0) {
        printf("Refreshing QWeather data...\n");
        esp_err_t err = weather_service_refresh_and_wait(45000);
        if (err != ESP_OK) {
            printf("weather refresh failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("Weather refresh complete.\n");
        return 0;
    }

    weather_snapshot_t *snap = calloc(1, sizeof(*snap));
    if (!snap) {
        printf("weather: no memory\n");
        return 1;
    }
    esp_err_t err = weather_service_get_snapshot(snap);
    if (err != ESP_OK) {
        printf("weather snapshot failed: %s\n", esp_err_to_name(err));
        free(snap);
        return 1;
    }

    if (strcmp(argv[1], "current") == 0) {
        if (!snap->current.valid) {
            printf("No current weather cached yet. Run: weather refresh\n");
            free(snap);
            return 1;
        }
        const weather_current_t *w = &snap->current;
        printf("Current weather (%s)\n", snap->provider[0] ? snap->provider : "unknown");
        printf("  condition : %s (%s)\n", w->condition_text, w->condition_code);
        printf("  temp      : %.1f C (feels %.1f C)\n", w->temperature_c, w->feels_like_c);
        printf("  humidity  : %.0f %%\n", w->humidity_pct);
        printf("  pressure  : %.1f hPa\n", w->pressure_hpa);
        printf("  wind      : %.1f m/s scale=%d dir=%d\n",
               w->wind_speed_mps, w->wind_scale, w->wind_direction_deg);
        printf("  precip    : %.2f mm intensity=%.2f mm/h\n",
               w->precipitation_mm, w->precipitation_intensity_mm_h);
        printf("  visibility: %.1f km\n", w->visibility_km);
        printf("  UV        : %.1f\n", w->uv_index);
        printf("  source    : %s\n", snap->attribution[0] ? snap->attribution : "(none)");
        free(snap);
        return 0;
    }

    if (strcmp(argv[1], "hourly") == 0) {
        int want = parse_count(argc, argv, 6, WEATHER_MAX_HOURLY);
        if (want < 0) {
            print_usage();
            free(snap);
            return 1;
        }
        if (snap->hourly_count == 0) {
            printf("No hourly forecast cached yet.\n");
            free(snap);
            return 1;
        }
        if ((size_t)want > snap->hourly_count) {
            want = (int)snap->hourly_count;
        }
        for (int i = 0; i < want; i++) {
            const weather_hourly_t *h = &snap->hourly[i];
            printf("%2d. %s  %-16s  %.1fC  rain %.0f%%  wind %.1fm/s\n",
                   i + 1, h->forecast_time, h->condition_text,
                   h->temperature_c, h->precipitation_probability_pct,
                   h->wind_speed_mps);
        }
        free(snap);
        return 0;
    }

    if (strcmp(argv[1], "daily") == 0) {
        int want = parse_count(argc, argv, WEATHER_MAX_DAILY, WEATHER_MAX_DAILY);
        if (want < 0) {
            print_usage();
            free(snap);
            return 1;
        }
        if (snap->daily_count == 0) {
            printf("No daily forecast cached yet.\n");
            free(snap);
            return 1;
        }
        if ((size_t)want > snap->daily_count) {
            want = (int)snap->daily_count;
        }
        for (int i = 0; i < want; i++) {
            const weather_daily_t *d = &snap->daily[i];
            printf("%2d. %s  %.1f..%.1fC  day=%s rain=%.0f%%  night=%s\n",
                   i + 1, d->forecast_start,
                   d->temperature_min_c, d->temperature_max_c,
                   d->daytime_text, d->daytime_precip_probability_pct,
                   d->nighttime_text);
        }
        free(snap);
        return 0;
    }

    if (strcmp(argv[1], "alerts") == 0) {
        if (snap->alert_count == 0) {
            printf("No active weather alerts.\n");
        } else {
            for (size_t i = 0; i < snap->alert_count; i++) {
                const weather_alert_t *a = &snap->alerts[i];
                printf("%u. [%s/%s] %s\n",
                       (unsigned)(i + 1), a->severity, a->color, a->headline);
                printf("   event=%s issuer=%s expire=%s\n",
                       a->event_name, a->sender, a->expire_time);
                if (a->description[0]) {
                    printf("   %s\n", a->description);
                }
            }
        }
        if (snap->alert_attribution[0]) {
            printf("source: %s\n", snap->alert_attribution);
        }
        free(snap);
        return 0;
    }

    free(snap);
    print_usage();
    return 1;
}

esp_err_t register_weather_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "weather",
        .help = "Weather V1: status/config/set/refresh/current/hourly/daily/alerts",
        .hint = NULL,
        .func = &cmd_weather,
        .argtable = NULL,
    };

    esp_err_t err = esp_console_cmd_register(&cmd);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "registered: weather");
    }
    return err;
}
