/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cmd_voice.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "voice_audio.h"

static const char *TAG = "cmd_voice";

#define VOICE_CMD_PATH_MAX            128
#define VOICE_CMD_DEFAULT_SECONDS     5U
#define VOICE_CMD_MAX_SECONDS         30U

static char s_storage_base[64];

static const char *voice_state_name(voice_audio_state_t state)
{
    switch (state) {
    case VOICE_AUDIO_STATE_UNINITIALIZED:
        return "uninitialized";
    case VOICE_AUDIO_STATE_IDLE:
        return "idle";
    case VOICE_AUDIO_STATE_CAPTURE:
        return "capture";
    case VOICE_AUDIO_STATE_PLAYBACK:
        return "playback";
    default:
        return "unknown";
    }
}

static esp_err_t ensure_audio_dir(char *wav_path, size_t wav_path_size)
{
    char audio_dir[VOICE_CMD_PATH_MAX];

    int n = snprintf(audio_dir, sizeof(audio_dir), "%s/audio", s_storage_base);
    if (n <= 0 || n >= (int)sizeof(audio_dir)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (mkdir(audio_dir, 0777) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed: %s", audio_dir, strerror(errno));
        return ESP_FAIL;
    }

    n = snprintf(wav_path, wav_path_size, "%s/voice_test.wav", audio_dir);
    if (n <= 0 || n >= (int)wav_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t parse_seconds(int argc, char **argv, uint32_t *seconds)
{
    *seconds = VOICE_CMD_DEFAULT_SECONDS;
    if (argc < 3) {
        return ESP_OK;
    }

    char *end = NULL;
    long parsed = strtol(argv[2], &end, 10);
    if (!end || *end != '\0' || parsed < 1 || parsed > VOICE_CMD_MAX_SECONDS) {
        return ESP_ERR_INVALID_ARG;
    }
    *seconds = (uint32_t)parsed;
    return ESP_OK;
}

static void print_usage(void)
{
    printf("Usage:\n");
    printf("  voice info\n");
    printf("  voice record [seconds]   (1-%u, default %u)\n",
           (unsigned)VOICE_CMD_MAX_SECONDS,
           (unsigned)VOICE_CMD_DEFAULT_SECONDS);
    printf("  voice play\n");
    printf("  voice test [seconds]     record then play\n");
    printf("  voice deinit\n");
}

static int cmd_voice(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "info") == 0) {
        voice_audio_info_t info = {0};
        voice_audio_get_info(&info);
        printf("Voice Audio V1\n");
        printf("  initialized : %s\n", info.initialized ? "yes" : "no");
        printf("  state       : %s\n", voice_state_name(info.state));
        printf("  sample rate : %u Hz\n", (unsigned)info.sample_rate_hz);
        printf("  PCM         : %u-bit mono\n", (unsigned)info.pcm_bits);
        printf("  BCLK        : GPIO%d\n", info.bclk_gpio);
        printf("  WS/LRCLK    : GPIO%d\n", info.ws_gpio);
        printf("  MIC SD      : GPIO%d\n", info.mic_data_gpio);
        printf("  SPK DIN     : GPIO%d\n", info.spk_data_gpio);
        printf("  storage     : %s\n", s_storage_base);
        return 0;
    }

    if (strcmp(argv[1], "deinit") == 0) {
        esp_err_t err = voice_audio_deinit();
        if (err != ESP_OK) {
            printf("voice deinit failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("voice audio deinitialized\n");
        return 0;
    }

    char wav_path[VOICE_CMD_PATH_MAX];
    esp_err_t err = ensure_audio_dir(wav_path, sizeof(wav_path));
    if (err != ESP_OK) {
        printf("audio directory failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    if (strcmp(argv[1], "play") == 0) {
        printf("Playing %s ...\n", wav_path);
        err = voice_audio_play_wav(wav_path);
        if (err != ESP_OK) {
            printf("voice play failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("Playback complete.\n");
        return 0;
    }

    if (strcmp(argv[1], "record") == 0 || strcmp(argv[1], "test") == 0) {
        uint32_t seconds = 0;
        err = parse_seconds(argc, argv, &seconds);
        if (err != ESP_OK) {
            print_usage();
            return 1;
        }

        printf("Voice capture starts in:\n");
        for (int i = 3; i > 0; i--) {
            printf("  %d\n", i);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        printf("Recording %u second(s) -> %s\n", (unsigned)seconds, wav_path);

        voice_audio_stats_t stats = {0};
        err = voice_audio_record_wav(wav_path, seconds * 1000U, &stats);
        if (err != ESP_OK) {
            printf("voice record failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        printf("Record complete: samples=%u pcm_bytes=%u peak=%d rms=%u\n",
               (unsigned)stats.samples,
               (unsigned)stats.pcm_bytes,
               (int)stats.peak,
               (unsigned)stats.rms);

        if (stats.peak < 64) {
            printf("WARNING: microphone level is almost zero; check SCK/WS/SD/LR/GND wiring.\n");
        } else if (stats.peak > 32000) {
            printf("WARNING: input is near clipping; move farther from the microphone if distorted.\n");
        }

        if (strcmp(argv[1], "test") == 0) {
            vTaskDelay(pdMS_TO_TICKS(300));
            printf("Playing recorded audio...\n");
            err = voice_audio_play_wav(wav_path);
            if (err != ESP_OK) {
                printf("voice test playback failed: %s\n", esp_err_to_name(err));
                return 1;
            }
            printf("Voice test complete.\n");
        }
        return 0;
    }

    print_usage();
    return 1;
}

esp_err_t register_voice_command(const char *storage_base_path)
{
    if (!storage_base_path || !storage_base_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlcpy(s_storage_base, storage_base_path, sizeof(s_storage_base)) >= sizeof(s_storage_base)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_console_cmd_t cmd = {
        .command = "voice",
        .help = "Voice Audio V1: info / record / play / test / deinit",
        .hint = NULL,
        .func = &cmd_voice,
        .argtable = NULL,
    };

    esp_err_t err = esp_console_cmd_register(&cmd);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "registered: voice (storage=%s)", s_storage_base);
    }
    return err;
}
