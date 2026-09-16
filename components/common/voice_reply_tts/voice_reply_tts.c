/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Asynchronous ESP-Claw final-reply -> ESP-TTS bridge.
 *
 * Design constraints:
 * - Never synthesize on the Event Router / HTTP task.
 * - Keep reply text in PSRAM.
 * - Use a transient task: no permanent internal-RAM task stack at idle.
 * - Never contend with an active realtime conversation for the shared I2S.
 */

#include "voice_reply_tts.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "voice_agent.h"
#include "voice_audio.h"
#include "voice_service.h"
#include "voice_tts.h"

static const char *TAG = "reply_tts";

#define REPLY_TTS_VOICE_DATA_PATH   "/sdcard/tts/voice_data.dat"
#define REPLY_TTS_SPEED             2U
#define REPLY_TTS_TEXT_MAX_BYTES    8192U
#define REPLY_TTS_TASK_STACK_BYTES  5120U
#define REPLY_TTS_TASK_PRIORITY     4
#define REPLY_TTS_WAIT_POLL_MS      100U
#define REPLY_TTS_REARM_GRACE_MS    250U

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_worker_running;
static char *s_pending_text;
static bool s_pending_force_restart;

static bool append_text(char *out, size_t cap, size_t *io_len, const char *text)
{
    size_t n = strlen(text);
    if (*io_len + n + 1U > cap) {
        return false;
    }
    memcpy(out + *io_len, text, n);
    *io_len += n;
    out[*io_len] = '\0';
    return true;
}

static bool append_byte(char *out, size_t cap, size_t *io_len, char c)
{
    if (*io_len + 2U > cap) {
        return false;
    }
    out[(*io_len)++] = c;
    out[*io_len] = '\0';
    return true;
}

static bool is_url_start(const char *s)
{
    return strncmp(s, "https://", 8) == 0 || strncmp(s, "http://", 7) == 0;
}

static char *reply_text_sanitize_psram(const char *text)
{
    if (!text || !text[0]) {
        return NULL;
    }

    size_t in_len = strnlen(text, REPLY_TTS_TEXT_MAX_BYTES + 1U);
    if (in_len == 0 || in_len > REPLY_TTS_TEXT_MAX_BYTES) {
        return NULL;
    }

    /*
     * Extra headroom is intentional because a few useful symbols are expanded
     * into spoken Chinese words, for example ℃ -> 摄氏度.
     */
    size_t cap = in_len * 4U + 64U;
    char *out = heap_caps_calloc(1, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        return NULL;
    }

    size_t i = 0;
    size_t o = 0;
    bool in_fenced_code = false;
    bool pending_space = false;

    while (i < in_len) {
        const char *s = text + i;

        /* Never read fenced code blocks aloud. */
        if (i + 3U <= in_len && strncmp(s, "```", 3) == 0) {
            in_fenced_code = !in_fenced_code;
            i += 3U;
            pending_space = true;
            continue;
        }
        if (in_fenced_code) {
            i++;
            continue;
        }

        /* Drop URLs instead of spelling them character by character. */
        if (is_url_start(s)) {
            while (i < in_len) {
                unsigned char c = (unsigned char)text[i];
                if (c == ' ' || c == '\r' || c == '\n' || c == '\t' ||
                    c == ')' || c == ']' || c == '}') {
                    break;
                }
                i++;
            }
            pending_space = true;
            continue;
        }

        /* Useful weather / numeric symbols become ordinary spoken words. */
        if (strncmp(s, "℃", strlen("℃")) == 0) {
            if (!append_text(out, cap, &o, "摄氏度")) break;
            i += strlen("℃");
            pending_space = false;
            continue;
        }
        if (strncmp(s, "°C", 3) == 0 || strncmp(s, "°c", 3) == 0) {
            if (!append_text(out, cap, &o, "摄氏度")) break;
            i += 3U;
            pending_space = false;
            continue;
        }
        if (strncmp(s, "°", strlen("°")) == 0) {
            if (!append_text(out, cap, &o, "度")) break;
            i += strlen("°");
            pending_space = false;
            continue;
        }

        /* Markdown / decorative Unicode punctuation: pause, but do not read it. */
        if (strncmp(s, "——", strlen("——")) == 0) {
            if (!append_text(out, cap, &o, "，")) break;
            i += strlen("——");
            pending_space = false;
            continue;
        }
        if (strncmp(s, "—", strlen("—")) == 0 ||
            strncmp(s, "–", strlen("–")) == 0 ||
            strncmp(s, "―", strlen("―")) == 0 ||
            strncmp(s, "…", strlen("…")) == 0 ||
            strncmp(s, "•", strlen("•")) == 0 ||
            strncmp(s, "·", strlen("·")) == 0) {
            if (!append_text(out, cap, &o, "，")) break;
            if (strncmp(s, "…", strlen("…")) == 0) i += strlen("…");
            else if (strncmp(s, "•", strlen("•")) == 0) i += strlen("•");
            else if (strncmp(s, "·", strlen("·")) == 0) i += strlen("·");
            else if (strncmp(s, "—", strlen("—")) == 0) i += strlen("—");
            else if (strncmp(s, "–", strlen("–")) == 0) i += strlen("–");
            else i += strlen("―");
            pending_space = false;
            continue;
        }

        /* Quotes/brackets are visual structure only; retain their inner text. */
        if (strncmp(s, "“", strlen("“")) == 0 ||
            strncmp(s, "”", strlen("”")) == 0 ||
            strncmp(s, "‘", strlen("‘")) == 0 ||
            strncmp(s, "’", strlen("’")) == 0 ||
            strncmp(s, "《", strlen("《")) == 0 ||
            strncmp(s, "》", strlen("》")) == 0) {
            if (strncmp(s, "“", strlen("“")) == 0) i += strlen("“");
            else if (strncmp(s, "”", strlen("”")) == 0) i += strlen("”");
            else if (strncmp(s, "‘", strlen("‘")) == 0) i += strlen("‘");
            else if (strncmp(s, "’", strlen("’")) == 0) i += strlen("’");
            else if (strncmp(s, "《", strlen("《")) == 0) i += strlen("《");
            else i += strlen("》");
            continue;
        }

        unsigned char c = (unsigned char)text[i];

        if (c == '\r' || c == '\n' || c == '\t') {
            if (!append_text(out, cap, &o, "，")) break;
            i++;
            pending_space = false;
            continue;
        }

        if (c == ' ') {
            pending_space = true;
            i++;
            continue;
        }

        /*
         * Make percentages natural: "80%" -> "百分之80".  ESP-TTS's number
         * reading is one of its clearest paths, so keep the digits unchanged.
         */
        if (c >= '0' && c <= '9') {
            size_t j = i;
            while (j < in_len &&
                   ((text[j] >= '0' && text[j] <= '9') || text[j] == '.')) {
                j++;
            }
            if (j < in_len && text[j] == '%') {
                if (!append_text(out, cap, &o, "百分之")) break;
                while (i < j) {
                    if (!append_byte(out, cap, &o, text[i])) break;
                    i++;
                }
                i = j + 1U;
                pending_space = false;
                continue;
            }
        }

        /*
         * Strip Markdown syntax. Parentheses/brackets are removed but their
         * readable contents remain. '#' '*' '`' etc. must never be spoken.
         */
        if (c == '*' || c == '#' || c == '`' || c == '_' || c == '~' ||
            c == '>' || c == '|' || c == '[' || c == ']' ||
            c == '(' || c == ')' || c == '{' || c == '}' || c == '\\') {
            i++;
            continue;
        }

        /* Preserve negative numbers as spoken "负", otherwise '-' is a pause. */
        if (c == '-') {
            bool next_digit = (i + 1U < in_len) &&
                              text[i + 1U] >= '0' && text[i + 1U] <= '9';
            if (next_digit) {
                if (!append_text(out, cap, &o, "负")) break;
            } else {
                if (!append_text(out, cap, &o, "，")) break;
            }
            i++;
            pending_space = false;
            continue;
        }

        if (c == '%') {
            if (!append_text(out, cap, &o, "百分号")) break;
            i++;
            pending_space = false;
            continue;
        }

        if (c == ':' || c == ';') {
            if (!append_text(out, cap, &o, "，")) break;
            i++;
            pending_space = false;
            continue;
        }

        if (c == ',') {
            if (!append_text(out, cap, &o, "，")) break;
            i++;
            pending_space = false;
            continue;
        }
        if (c == '.') {
            if (!append_text(out, cap, &o, "。")) break;
            i++;
            pending_space = false;
            continue;
        }
        if (c == '?') {
            if (!append_text(out, cap, &o, "？")) break;
            i++;
            pending_space = false;
            continue;
        }
        if (c == '!') {
            if (!append_text(out, cap, &o, "！")) break;
            i++;
            pending_space = false;
            continue;
        }

        /* Drop most emoji (4-byte UTF-8) rather than feeding them to ESP-TTS. */
        if ((c & 0xF8U) == 0xF0U && i + 3U < in_len) {
            i += 4U;
            pending_space = true;
            continue;
        }

        if (pending_space) {
            if (o > 0 && !append_byte(out, cap, &o, ' ')) break;
            pending_space = false;
        }

        /*
         * Copy ordinary UTF-8 bytes unchanged. This keeps Chinese, numbers,
         * Latin letters and normal Chinese punctuation on the same ESP-TTS
         * parser path.
         */
        if (!append_byte(out, cap, &o, (char)c)) {
            break;
        }
        i++;
    }

    while (o > 0 && (out[o - 1U] == ' ' || out[o - 1U] == '\r' ||
                     out[o - 1U] == '\n' || out[o - 1U] == '\t')) {
        out[--o] = '\0';
    }

    if (o == 0) {
        free(out);
        return NULL;
    }

    ESP_LOGI(TAG, "speech text sanitized in=%u out=%u text='%s'",
             (unsigned)in_len, (unsigned)o, out);
    return out;
}

static char *reply_take_pending(bool *out_force_restart)
{
    char *text = NULL;
    bool force_restart = false;

    portENTER_CRITICAL(&s_mux);
    text = s_pending_text;
    force_restart = s_pending_force_restart;
    s_pending_text = NULL;
    s_pending_force_restart = false;
    if (!text) {
        s_worker_running = false;
    }
    portEXIT_CRITICAL(&s_mux);

    if (out_force_restart) {
        *out_force_restart = force_restart;
    }
    return text;
}

static void wait_for_dialog_idle(void)
{
    bool waited = false;

    for (;;) {
        if (voice_agent_is_active()) {
            if (!waited) {
                ESP_LOGI(TAG,
                         "realtime conversation active; keeping web reply queued");
                waited = true;
            }
            vTaskDelay(pdMS_TO_TICKS(REPLY_TTS_WAIT_POLL_MS));
            continue;
        }

        if (!waited) {
            return;
        }

        /*
         * voice_dialog re-arms WakeNet shortly after the cloud session closes.
         * Let that transition settle before deciding whether Voice Service must
         * be paused for this queued local reply.
         */
        vTaskDelay(pdMS_TO_TICKS(REPLY_TTS_REARM_GRACE_MS));
        if (!voice_agent_is_active()) {
            ESP_LOGI(TAG, "realtime conversation ended; queued reply may play");
            return;
        }
    }
}

static void wait_for_playback_window(bool *io_restart_voice)
{
    bool audio_wait_logged = false;

    for (;;) {
        wait_for_dialog_idle();

        if (voice_service_is_running()) {
            ESP_LOGI(TAG, "pausing Voice Service for queued reply TTS");
            esp_err_t stop_err = voice_service_stop();
            if (stop_err != ESP_OK) {
                ESP_LOGW(TAG,
                         "Voice Service stop pending: %s; reply remains queued",
                         esp_err_to_name(stop_err));
                vTaskDelay(pdMS_TO_TICKS(REPLY_TTS_WAIT_POLL_MS));
                continue;
            }
            *io_restart_voice = true;
        }

        /* A session can become active while Voice Service is winding down. */
        if (voice_agent_is_active()) {
            continue;
        }

        voice_audio_info_t info = {0};
        voice_audio_get_info(&info);
        if (!info.initialized || info.state == VOICE_AUDIO_STATE_IDLE) {
            return;
        }

        if (!audio_wait_logged) {
            ESP_LOGI(TAG,
                     "I2S owner state=%d; keeping reply queued until audio is idle",
                     (int)info.state);
            audio_wait_logged = true;
        }
        vTaskDelay(pdMS_TO_TICKS(REPLY_TTS_WAIT_POLL_MS));
    }
}

static void reply_tts_worker(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG,
             "worker start internal_free=%u largest=%u psram_free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    for (;;) {
        bool force_restart = false;
        char *text = reply_take_pending(&force_restart);
        if (!text) {
            break;
        }

        /*
         * This worker is launched from the Web IM outbound callback.
         * Give the Event Router / final-response persistence path a short
         * window to return before touching SD/I2S/TTS resources.
         */
        /*
         * Voice-dialog replies are already detached from the capture task, so
         * they only need a tiny handoff window. Web replies keep the original
         * delay to let the Web IM/event path return cleanly.
         */
        vTaskDelay(pdMS_TO_TICKS(force_restart ? 80U : 350U));

        voice_tts_result_t result = {0};
        bool restart_voice = force_restart;
        esp_err_t tts_err = ESP_OK;

        for (;;) {
            wait_for_playback_window(&restart_voice);

            ESP_LOGI(TAG, "speak queued reply bytes=%u speed=%u",
                     (unsigned)strlen(text), (unsigned)REPLY_TTS_SPEED);

            tts_err = voice_tts_speak_file(REPLY_TTS_VOICE_DATA_PATH,
                                           text,
                                           REPLY_TTS_SPEED,
                                           &result);
            if (tts_err != ESP_ERR_INVALID_STATE) {
                break;
            }

            /*
             * The final idle observation and playback_start are necessarily
             * separate calls.  voice_audio now arbitrates playback_start
             * atomically, so losing that race is harmless: retain and retry.
             */
            ESP_LOGI(TAG, "audio owner changed; reply remains queued for retry");
            vTaskDelay(pdMS_TO_TICKS(REPLY_TTS_WAIT_POLL_MS));
        }

        if (tts_err == ESP_OK) {
            ESP_LOGI(TAG,
                     "reply complete pcm_ms=%u samples=%llu internal_free=%u psram_free=%u",
                     (unsigned)result.pcm_ms,
                     (unsigned long long)result.pcm_samples,
                     (unsigned)result.internal_free_after,
                     (unsigned)result.psram_free_after);
        } else {
            ESP_LOGW(TAG, "reply TTS failed: %s", esp_err_to_name(tts_err));
        }

        free(text);

        if (restart_voice) {
            esp_err_t start_err = voice_service_start();
            if (start_err == ESP_OK || start_err == ESP_ERR_INVALID_STATE) {
                ESP_LOGI(TAG, "Voice Service restarted after reply TTS");
            } else {
                ESP_LOGW(TAG,
                         "Voice Service restart failed: %s",
                         esp_err_to_name(start_err));
            }
        }
    }

    ESP_LOGI(TAG,
             "worker exit stack_hwm=%u internal_free=%u largest=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    vTaskDelete(NULL);
}

static esp_err_t voice_reply_tts_enqueue_internal(const char *utf8_text,
                                                  bool force_restart)
{
    if (!utf8_text || !utf8_text[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strnlen(utf8_text, REPLY_TTS_TEXT_MAX_BYTES + 1U);
    if (len == 0 || len > REPLY_TTS_TEXT_MAX_BYTES) {
        ESP_LOGW(TAG, "reply too large: >%u bytes",
                 (unsigned)REPLY_TTS_TEXT_MAX_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    char *copy = reply_text_sanitize_psram(utf8_text);
    if (!copy) {
        ESP_LOGW(TAG, "reply sanitize/copy failed bytes=%u", (unsigned)len);
        return ESP_ERR_NO_MEM;
    }

    char *old_pending = NULL;
    bool create_worker = false;

    portENTER_CRITICAL(&s_mux);
    old_pending = s_pending_text;
    s_pending_text = copy;
    s_pending_force_restart = s_pending_force_restart || force_restart;
    if (!s_worker_running) {
        s_worker_running = true;
        create_worker = true;
    }
    portEXIT_CRITICAL(&s_mux);

    free(old_pending);

    if (!create_worker) {
        ESP_LOGI(TAG,
                 "reply queued/replaced pending bytes=%u restart=%d",
                 (unsigned)len,
                 force_restart);
        return ESP_OK;
    }

    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(reply_tts_worker,
                                                 "reply_tts",
                                                 REPLY_TTS_TASK_STACK_BYTES,
                                                 NULL,
                                                 REPLY_TTS_TASK_PRIORITY,
                                                 &task,
                                                 tskNO_AFFINITY);
    if (created != pdPASS) {
        char *orphan = NULL;

        portENTER_CRITICAL(&s_mux);
        orphan = s_pending_text;
        s_pending_text = NULL;
        s_pending_force_restart = false;
        s_worker_running = false;
        portEXIT_CRITICAL(&s_mux);

        free(orphan);
        ESP_LOGW(TAG,
                 "worker create failed stack=%u internal_free=%u largest=%u",
                 (unsigned)REPLY_TTS_TASK_STACK_BYTES,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "reply accepted bytes=%u worker_stack=%u restart=%d",
             (unsigned)len,
             (unsigned)REPLY_TTS_TASK_STACK_BYTES,
             force_restart);
    return ESP_OK;
}

esp_err_t voice_reply_tts_preload(void)
{
    ESP_LOGI(TAG, "preloading local ESP-TTS voice data");
    return voice_tts_preload_file(REPLY_TTS_VOICE_DATA_PATH);
}

esp_err_t voice_reply_tts_enqueue(const char *utf8_text)
{
    return voice_reply_tts_enqueue_internal(utf8_text, false);
}

esp_err_t voice_reply_tts_enqueue_and_restart(const char *utf8_text)
{
    return voice_reply_tts_enqueue_internal(utf8_text, true);
}

bool voice_reply_tts_is_busy(void)
{
    bool busy;

    portENTER_CRITICAL(&s_mux);
    busy = s_worker_running || s_pending_text != NULL;
    portEXIT_CRITICAL(&s_mux);

    return busy;
}
