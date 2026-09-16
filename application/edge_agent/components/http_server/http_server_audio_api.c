/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Web Audio Debug + Voice Service V1 control API.
 *
 * POST /api/audio/info
 * POST /api/audio/record?seconds=3
 * POST /api/audio/play
 * POST /api/audio/test?seconds=3
 * POST /api/audio/voice/start
 * POST /api/audio/voice/stop
 * POST /api/audio/voice/status
 * POST /api/audio/voice/ask   JSON: {"text":"...","session":"optional"}
 * POST /api/audio/asr/config  JSON: {"api_key":"...","base_url":"...","model":"..."}
 */

#include "http_server_priv.h"
#include "app_claw.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "voice_audio.h"
#include "voice_service.h"
#include "voice_dialog.h"
#include "voice_tts.h"

static const char *TAG = "http_audio";

#define AUDIO_PATH_MAX         192
#define AUDIO_DEFAULT_SECONDS  3U
#define AUDIO_MAX_SECONDS      10U

#define CLAW_ASK_TEXT_MAX       768U
#define CLAW_ASK_SESSION_MAX    64U
#define CLAW_REPLY_MAX          6144U
#define CLAW_REPLY_TIMEOUT_MS   130000U
#define CLAW_TTS_SPEED          2U

static portMUX_TYPE s_claw_ask_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_claw_ask_busy;

static bool claw_ask_try_lock(void)
{
    bool ok = false;
    portENTER_CRITICAL(&s_claw_ask_mux);
    if (!s_claw_ask_busy) {
        s_claw_ask_busy = true;
        ok = true;
    }
    portEXIT_CRITICAL(&s_claw_ask_mux);
    return ok;
}

static void claw_ask_unlock(void)
{
    portENTER_CRITICAL(&s_claw_ask_mux);
    s_claw_ask_busy = false;
    portEXIT_CRITICAL(&s_claw_ask_mux);
}

static void log_heap_point(const char *where)
{
    ESP_LOGI(TAG,
             "%s: internal_free=%u largest=%u psram_free=%u",
             where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static const char *audio_state_name(voice_audio_state_t state)
{
    switch (state) {
    case VOICE_AUDIO_STATE_UNINITIALIZED: return "uninitialized";
    case VOICE_AUDIO_STATE_IDLE:          return "idle";
    case VOICE_AUDIO_STATE_CAPTURE:       return "capture";
    case VOICE_AUDIO_STATE_PLAYBACK:      return "playback";
    default:                              return "unknown";
    }
}

static esp_err_t build_wav_path(char *wav_path, size_t wav_path_size)
{
    const http_server_ctx_t *ctx = http_server_ctx();
    char audio_dir[AUDIO_PATH_MAX];

    int n = snprintf(audio_dir, sizeof(audio_dir), "%s/audio", ctx->storage_base_path);
    ESP_RETURN_ON_FALSE(n > 0 && n < (int)sizeof(audio_dir),
                        ESP_ERR_INVALID_SIZE, TAG, "audio directory path too long");

    if (mkdir(audio_dir, 0777) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed: %s", audio_dir, strerror(errno));
        return ESP_FAIL;
    }

    n = snprintf(wav_path, wav_path_size, "%s/voice_test.wav", audio_dir);
    ESP_RETURN_ON_FALSE(n > 0 && n < (int)wav_path_size,
                        ESP_ERR_INVALID_SIZE, TAG, "WAV path too long");
    return ESP_OK;
}

static uint32_t query_seconds(httpd_req_t *req)
{
    char value[16] = {0};
    if (http_server_query_get(req, "seconds", value, sizeof(value)) != ESP_OK || !value[0]) {
        return AUDIO_DEFAULT_SECONDS;
    }

    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (!end || *end != '\0' || v < 1 || v > AUDIO_MAX_SECONDS) {
        return 0;
    }
    return (uint32_t)v;
}

static esp_err_t send_error_json(httpd_req_t *req, const char *action, esp_err_t err)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }

    cJSON_AddBoolToObject(root, "ok", false);
    http_server_json_add_string(root, "action", action);
    http_server_json_add_string(root, "error", esp_err_to_name(err));
    return http_server_send_json_response(req, root);
}

static esp_err_t send_busy_json(httpd_req_t *req, const char *action)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    cJSON_AddBoolToObject(root, "ok", false);
    http_server_json_add_string(root, "action", action);
    http_server_json_add_string(root, "error", "VOICE_SERVICE_RUNNING");
    http_server_json_add_string(root, "hint", "POST /api/audio/voice/stop first");
    return http_server_send_json_response(req, root);
}

static esp_err_t send_info(httpd_req_t *req)
{
    voice_audio_info_t info = {0};
    voice_audio_get_info(&info);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }

    cJSON_AddBoolToObject(root, "ok", true);
    http_server_json_add_string(root, "action", "info");
    cJSON_AddBoolToObject(root, "initialized", info.initialized);
    http_server_json_add_string(root, "state", audio_state_name(info.state));
    cJSON_AddNumberToObject(root, "sample_rate_hz", info.sample_rate_hz);
    cJSON_AddNumberToObject(root, "pcm_bits", info.pcm_bits);
    cJSON_AddNumberToObject(root, "bclk_gpio", info.bclk_gpio);
    cJSON_AddNumberToObject(root, "ws_gpio", info.ws_gpio);
    cJSON_AddNumberToObject(root, "mic_data_gpio", info.mic_data_gpio);
    cJSON_AddNumberToObject(root, "spk_data_gpio", info.spk_data_gpio);
    cJSON_AddBoolToObject(root, "voice_service_running", voice_service_is_running());
    return http_server_send_json_response(req, root);
}

static esp_err_t do_record(httpd_req_t *req, bool play_after)
{
    if (voice_service_is_running()) {
        return send_busy_json(req, play_after ? "test" : "record");
    }

    char wav_path[AUDIO_PATH_MAX];
    uint32_t seconds = query_seconds(req);
    if (seconds == 0) {
        return send_error_json(req, play_after ? "test" : "record", ESP_ERR_INVALID_ARG);
    }

    esp_err_t err = build_wav_path(wav_path, sizeof(wav_path));
    if (err != ESP_OK) {
        return send_error_json(req, play_after ? "test" : "record", err);
    }

    ESP_LOGI(TAG, "%s start seconds=%u path=%s",
             play_after ? "test" : "record", (unsigned)seconds, wav_path);

    voice_audio_stats_t stats = {0};
    err = voice_audio_record_wav(wav_path, seconds * 1000U, &stats);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "record failed: %s", esp_err_to_name(err));
        return send_error_json(req, play_after ? "test" : "record", err);
    }

    if (play_after) {
        err = voice_audio_play_wav(wav_path);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "playback after record failed: %s", esp_err_to_name(err));
            return send_error_json(req, "test", err);
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }

    cJSON_AddBoolToObject(root, "ok", true);
    http_server_json_add_string(root, "action", play_after ? "test" : "record");
    http_server_json_add_string(root, "path", wav_path);
    cJSON_AddNumberToObject(root, "seconds", seconds);
    cJSON_AddNumberToObject(root, "samples", stats.samples);
    cJSON_AddNumberToObject(root, "pcm_bytes", stats.pcm_bytes);
    cJSON_AddNumberToObject(root, "peak", stats.peak);
    cJSON_AddNumberToObject(root, "rms", stats.rms);
    cJSON_AddBoolToObject(root, "played", play_after);
    return http_server_send_json_response(req, root);
}

static esp_err_t do_play(httpd_req_t *req)
{
    if (voice_service_is_running()) {
        return send_busy_json(req, "play");
    }

    char wav_path[AUDIO_PATH_MAX];
    esp_err_t err = build_wav_path(wav_path, sizeof(wav_path));
    if (err != ESP_OK) {
        return send_error_json(req, "play", err);
    }

    ESP_LOGI(TAG, "play start path=%s", wav_path);
    err = voice_audio_play_wav(wav_path);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "play failed: %s", esp_err_to_name(err));
        return send_error_json(req, "play", err);
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }

    cJSON_AddBoolToObject(root, "ok", true);
    http_server_json_add_string(root, "action", "play");
    http_server_json_add_string(root, "path", wav_path);
    return http_server_send_json_response(req, root);
}

static esp_err_t send_voice_status(httpd_req_t *req, const char *action, esp_err_t action_err)
{
    voice_service_status_t status = {0};
    voice_service_get_status(&status);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }

    cJSON_AddBoolToObject(root, "ok", action_err == ESP_OK);
    http_server_json_add_string(root, "action", action);
    if (action_err != ESP_OK) {
        http_server_json_add_string(root, "error", esp_err_to_name(action_err));
    }
    cJSON_AddBoolToObject(root, "running", status.running);
    http_server_json_add_string(root, "state", voice_service_state_name(status.state));
    http_server_json_add_string(root, "last_error", esp_err_to_name(status.last_error));
    http_server_json_add_string(root, "model", status.model_name);
    http_server_json_add_string(root, "wake_word", status.wake_word);
    cJSON_AddNumberToObject(root, "sample_rate_hz", status.sample_rate_hz);
    cJSON_AddNumberToObject(root, "frame_samples", status.frame_samples);
    cJSON_AddNumberToObject(root, "wake_count", status.wake_count);
    cJSON_AddNumberToObject(root, "utterance_count", status.utterance_count);
    cJSON_AddNumberToObject(root, "noise_rms", status.noise_rms);
    cJSON_AddNumberToObject(root, "last_rms", status.last_rms);
    cJSON_AddNumberToObject(root, "last_peak", status.last_peak);
    cJSON_AddNumberToObject(root, "last_utterance_ms", status.last_utterance_ms);
    cJSON_AddNumberToObject(root, "task_stack_free_bytes", status.task_stack_high_water_bytes);
    cJSON_AddNumberToObject(root, "internal_free", status.internal_free);
    cJSON_AddNumberToObject(root, "internal_largest", status.internal_largest);
    cJSON_AddNumberToObject(root, "psram_free", status.psram_free);
    return http_server_send_json_response(req, root);
}


#define TTS_TEXT_MAX 256

static esp_err_t build_tts_voice_path(char *path, size_t path_size)
{
    const http_server_ctx_t *ctx = http_server_ctx();
    int n = snprintf(path, path_size, "%s/tts/voice_data.dat", ctx->storage_base_path);
    ESP_RETURN_ON_FALSE(n > 0 && n < (int)path_size,
                        ESP_ERR_INVALID_SIZE, TAG, "TTS voice-data path too long");
    return ESP_OK;
}

static esp_err_t do_tts_status(httpd_req_t *req)
{
    char path[AUDIO_PATH_MAX];
    esp_err_t err = build_tts_voice_path(path, sizeof(path));
    if (err != ESP_OK) {
        return send_error_json(req, "tts_status", err);
    }

    struct stat st = {0};
    bool present = (stat(path, &st) == 0 && S_ISREG(st.st_mode));

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    http_server_json_add_string(root, "action", "tts_status");
    http_server_json_add_string(root, "voice_data_path", path);
    cJSON_AddBoolToObject(root, "voice_data_present", present);
    cJSON_AddNumberToObject(root, "voice_data_bytes", present ? (double)st.st_size : 0);
    cJSON_AddBoolToObject(root, "voice_service_running", voice_service_is_running());
    cJSON_AddNumberToObject(root, "internal_free",
                            heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "internal_largest",
                            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "psram_free",
                            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return http_server_send_json_response(req, root);
}

static esp_err_t do_tts_test(httpd_req_t *req)
{
    if (voice_service_is_running()) {
        return send_busy_json(req, "tts_test");
    }

    char voice_path[AUDIO_PATH_MAX];
    esp_err_t err = build_tts_voice_path(voice_path, sizeof(voice_path));
    if (err != ESP_OK) {
        return send_error_json(req, "tts_test", err);
    }

    char text[TTS_TEXT_MAX] = "你好我是小智本地语音合成测试成功";
    char query_text[TTS_TEXT_MAX] = {0};
    if (http_server_query_get(req, "text", query_text, sizeof(query_text)) == ESP_OK &&
        query_text[0]) {
        strlcpy(text, query_text, sizeof(text));
    }

    voice_tts_result_t result = {0};
    err = voice_tts_speak_file(voice_path, text, 3, &result);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS test failed: %s", esp_err_to_name(err));
        return send_error_json(req, "tts_test", err);
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    http_server_json_add_string(root, "action", "tts_test");
    http_server_json_add_string(root, "text", text);
    http_server_json_add_string(root, "voice_data_path", voice_path);
    cJSON_AddNumberToObject(root, "voice_data_bytes", (double)result.voice_data_bytes);
    cJSON_AddNumberToObject(root, "pcm_samples", (double)result.pcm_samples);
    cJSON_AddNumberToObject(root, "pcm_ms", result.pcm_ms);
    cJSON_AddNumberToObject(root, "internal_free", result.internal_free_after);
    cJSON_AddNumberToObject(root, "psram_free", result.psram_free_after);
    return http_server_send_json_response(req, root);
}


static esp_err_t send_claw_busy_json(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    cJSON_AddBoolToObject(root, "ok", false);
    http_server_json_add_string(root, "action", "claw_tts_ask");
    http_server_json_add_string(root, "error", "CLAW_ASK_BUSY");
    return http_server_send_json_response(req, root);
}

static esp_err_t read_claw_ask_input(httpd_req_t *req,
                                     char *text,
                                     size_t text_size,
                                     char *session,
                                     size_t session_size)
{
    ESP_RETURN_ON_FALSE(req && text && text_size > 1 && session && session_size > 1,
                        ESP_ERR_INVALID_ARG, TAG, "invalid ask input buffers");

    text[0] = '\0';
    session[0] = '\0';

    if (req->content_len > 0) {
        cJSON *body = NULL;
        esp_err_t err = http_server_parse_json_body(req, &body);
        if (err != ESP_OK) {
            return err;
        }

        cJSON *text_item = cJSON_GetObjectItemCaseSensitive(body, "text");
        cJSON *session_item = cJSON_GetObjectItemCaseSensitive(body, "session");

        if (cJSON_IsString(text_item) && text_item->valuestring) {
            size_t len = strlen(text_item->valuestring);
            if (len + 1 > text_size) {
                cJSON_Delete(body);
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(text, text_item->valuestring, len + 1);
        }

        if (cJSON_IsString(session_item) && session_item->valuestring &&
            session_item->valuestring[0]) {
            size_t len = strlen(session_item->valuestring);
            if (len + 1 > session_size) {
                cJSON_Delete(body);
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(session, session_item->valuestring, len + 1);
        }

        cJSON_Delete(body);
    } else {
        esp_err_t err = http_server_query_get(req, "text", text, text_size);
        if (err != ESP_OK) {
            return err;
        }
        (void)http_server_query_get(req, "session", session, session_size);
    }

    return text[0] ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t do_claw_tts_ask(httpd_req_t *req)
{
    if (voice_service_is_running()) {
        return send_busy_json(req, "claw_tts_ask");
    }
    if (!app_claw_get_core()) {
        return send_error_json(req, "claw_tts_ask", ESP_ERR_INVALID_STATE);
    }
    if (!claw_ask_try_lock()) {
        return send_claw_busy_json(req);
    }

    esp_err_t err = ESP_OK;
    esp_err_t send_err = ESP_OK;
    char voice_path[AUDIO_PATH_MAX] = {0};
    char question[CLAW_ASK_TEXT_MAX] = {0};
    char session[CLAW_ASK_SESSION_MAX] = {0};
    char *reply = NULL;
    voice_tts_result_t tts_result = {0};
    uint32_t llm_ms = 0;

    err = build_tts_voice_path(voice_path, sizeof(voice_path));
    if (err != ESP_OK) {
        send_err = send_error_json(req, "claw_tts_ask", err);
        goto cleanup;
    }

    struct stat st = {0};
    if (stat(voice_path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        ESP_LOGE(TAG, "Claw+TTS voice data missing/empty: %s", voice_path);
        send_err = send_error_json(req, "claw_tts_ask", ESP_ERR_NOT_FOUND);
        goto cleanup;
    }

    err = read_claw_ask_input(req,
                              question, sizeof(question),
                              session, sizeof(session));
    if (err != ESP_OK) {
        send_err = send_error_json(req, "claw_tts_ask", err);
        goto cleanup;
    }

    reply = heap_caps_calloc(1, CLAW_REPLY_MAX,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!reply) {
        send_err = send_error_json(req, "claw_tts_ask", ESP_ERR_NO_MEM);
        goto cleanup;
    }

    ESP_LOGI(TAG, "CLAW+TTS ask start text_bytes=%u session=%s",
             (unsigned)strlen(question),
             session[0] ? session : "(single-turn)");
    log_heap_point("before Claw ask");

    int64_t llm_start_us = esp_timer_get_time();
    err = app_claw_ask_text(question,
                            session[0] ? session : NULL,
                            reply,
                            CLAW_REPLY_MAX,
                            CLAW_REPLY_TIMEOUT_MS);
    llm_ms = (uint32_t)((esp_timer_get_time() - llm_start_us) / 1000);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Claw ask failed after %u ms: %s",
                 (unsigned)llm_ms, esp_err_to_name(err));
        send_err = send_error_json(req, "claw_tts_ask", err);
        goto cleanup;
    }

    ESP_LOGI(TAG, "CLAW reply ready bytes=%u llm_ms=%u",
             (unsigned)strlen(reply), (unsigned)llm_ms);
    log_heap_point("after Claw reply copy");

    err = voice_tts_speak_file(voice_path, reply, CLAW_TTS_SPEED, &tts_result);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Claw reply TTS failed: %s", esp_err_to_name(err));
        send_err = send_error_json(req, "claw_tts_ask", err);
        goto cleanup;
    }

    ESP_LOGI(TAG, "CLAW+TTS complete llm_ms=%u tts_ms=%u",
             (unsigned)llm_ms, (unsigned)tts_result.pcm_ms);

    {
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            send_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
            goto cleanup;
        }

        cJSON_AddBoolToObject(root, "ok", true);
        http_server_json_add_string(root, "action", "claw_tts_ask");
        http_server_json_add_string(root, "question", question);
        http_server_json_add_string(root, "answer", reply);
        if (session[0]) {
            http_server_json_add_string(root, "session", session);
        }
        cJSON_AddNumberToObject(root, "llm_ms", llm_ms);
        cJSON_AddNumberToObject(root, "tts_speed", CLAW_TTS_SPEED);
        cJSON_AddNumberToObject(root, "tts_pcm_samples", (double)tts_result.pcm_samples);
        cJSON_AddNumberToObject(root, "tts_pcm_ms", tts_result.pcm_ms);
        cJSON_AddNumberToObject(root, "voice_data_bytes",
                                (double)tts_result.voice_data_bytes);
        cJSON_AddNumberToObject(root, "internal_free",
                                heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(root, "psram_free",
                                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        send_err = http_server_send_json_response(req, root);
    }

cleanup:
    free(reply);
    claw_ask_unlock();
    return send_err;
}


static esp_err_t send_asr_config_json(httpd_req_t *req,
                                      const voice_dialog_asr_config_t *config,
                                      esp_err_t action_err)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }

    cJSON_AddBoolToObject(root, "ok", action_err == ESP_OK);
    http_server_json_add_string(root, "action", "asr_config");
    if (action_err != ESP_OK) {
        http_server_json_add_string(root, "error", esp_err_to_name(action_err));
    }
    cJSON_AddBoolToObject(root, "configured", config && config->api_key[0]);
    http_server_json_add_string(root,
                                "base_url",
                                (config && config->base_url[0]) ? config->base_url : "");
    http_server_json_add_string(root,
                                "model",
                                (config && config->model[0]) ? config->model : "");
    cJSON_AddBoolToObject(root, "dialog_busy", voice_dialog_is_busy());
    return http_server_send_json_response(req, root);
}

static esp_err_t do_asr_config(httpd_req_t *req)
{
    voice_dialog_asr_config_t config = {0};
    esp_err_t err = voice_dialog_get_asr_config(&config);
    if (err != ESP_OK) {
        return send_asr_config_json(req, &config, err);
    }

    /* POST with no body is a safe status/read operation. */
    if (req->content_len <= 0) {
        return send_asr_config_json(req, &config, ESP_OK);
    }

    cJSON *body = NULL;
    err = http_server_parse_json_body(req, &body);
    if (err != ESP_OK) {
        return send_asr_config_json(req, &config, err);
    }

    cJSON *api_key = cJSON_GetObjectItemCaseSensitive(body, "api_key");
    cJSON *base_url = cJSON_GetObjectItemCaseSensitive(body, "base_url");
    cJSON *model = cJSON_GetObjectItemCaseSensitive(body, "model");

    if (api_key && !cJSON_IsString(api_key)) {
        cJSON_Delete(body);
        return send_asr_config_json(req, &config, ESP_ERR_INVALID_ARG);
    }
    if (base_url && !cJSON_IsString(base_url)) {
        cJSON_Delete(body);
        return send_asr_config_json(req, &config, ESP_ERR_INVALID_ARG);
    }
    if (model && !cJSON_IsString(model)) {
        cJSON_Delete(body);
        return send_asr_config_json(req, &config, ESP_ERR_INVALID_ARG);
    }

    if (cJSON_IsString(api_key)) {
        if (strlcpy(config.api_key,
                    api_key->valuestring ? api_key->valuestring : "",
                    sizeof(config.api_key)) >= sizeof(config.api_key)) {
            cJSON_Delete(body);
            return send_asr_config_json(req, &config, ESP_ERR_INVALID_SIZE);
        }
    }
    if (cJSON_IsString(base_url)) {
        if (strlcpy(config.base_url,
                    base_url->valuestring ? base_url->valuestring : "",
                    sizeof(config.base_url)) >= sizeof(config.base_url)) {
            cJSON_Delete(body);
            return send_asr_config_json(req, &config, ESP_ERR_INVALID_SIZE);
        }
    }
    if (cJSON_IsString(model)) {
        if (strlcpy(config.model,
                    model->valuestring ? model->valuestring : "",
                    sizeof(config.model)) >= sizeof(config.model)) {
            cJSON_Delete(body);
            return send_asr_config_json(req, &config, ESP_ERR_INVALID_SIZE);
        }
    }

    cJSON_Delete(body);
    err = voice_dialog_set_asr_config(&config);
    if (err != ESP_OK) {
        return send_asr_config_json(req, &config, err);
    }

    ESP_LOGI(TAG,
             "ASR config saved configured=%d model=%s url=%s",
             config.api_key[0] != '\0',
             config.model,
             config.base_url);
    return send_asr_config_json(req, &config, ESP_OK);
}

static esp_err_t audio_api_handler(httpd_req_t *req)
{
    if (strcmp(req->uri, "/api/audio/info") == 0) {
        return send_info(req);
    }
    if (strcmp(req->uri, "/api/audio/asr/config") == 0) {
        return do_asr_config(req);
    }
    if (strncmp(req->uri, "/api/audio/voice/ask",
                strlen("/api/audio/voice/ask")) == 0) {
        return do_claw_tts_ask(req);
    }
    if (strcmp(req->uri, "/api/audio/voice/status") == 0) {
        return send_voice_status(req, "voice_status", ESP_OK);
    }
    if (strcmp(req->uri, "/api/audio/voice/start") == 0) {
        esp_err_t err = voice_service_start();
        return send_voice_status(req, "voice_start", err);
    }
    if (strcmp(req->uri, "/api/audio/voice/stop") == 0) {
        esp_err_t err = voice_service_stop();
        return send_voice_status(req, "voice_stop", err);
    }
    if (strcmp(req->uri, "/api/audio/tts/status") == 0) {
        return do_tts_status(req);
    }
    if (strncmp(req->uri, "/api/audio/tts/test", strlen("/api/audio/tts/test")) == 0) {
        return do_tts_test(req);
    }
    if (strncmp(req->uri, "/api/audio/record", strlen("/api/audio/record")) == 0) {
        return do_record(req, false);
    }
    if (strcmp(req->uri, "/api/audio/play") == 0) {
        return do_play(req);
    }
    if (strncmp(req->uri, "/api/audio/test", strlen("/api/audio/test")) == 0) {
        return do_record(req, true);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
}

esp_err_t http_server_register_audio_routes(httpd_handle_t server)
{
    const httpd_uri_t route = {
        .uri = "/api/audio/*",
        .method = HTTP_POST,
        .handler = audio_api_handler,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(server, &route);
}
