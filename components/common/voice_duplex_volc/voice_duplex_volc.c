/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-Claw Doubao Seeduplex product transport.
 *
 * Protocol source of truth:
 *   wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue
 *   X-Api-Key authentication
 *   session.create
 *   input_audio_buffer.append (base64 PCM16/16 kHz, 20 ms / 640 bytes pacing)
 *   input_audio_buffer.commit (one event after the local VAD-bounded turn)
 *   response.output_audio.delta (base64 PCM16LE/24 kHz, requested as pcm_s16le)
 *
 * The existing ESP-Claw audio HAL is intentionally left at its validated 16 kHz
 * format. Downlink 24 kHz PCM is downsampled to 16 kHz before MAX98357A output.
 * The Voice Service streams one dynamically VAD-bounded microphone turn. The
 * transport drains that PCM, commits the cloud input once, then remains silent
 * while receiving and playing the response. After playback has drained, the
 * same cloud session is reused for the next local turn.
 */

#include "voice_duplex_volc.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "settings_store.h"
#include "voice_audio.h"

static const char *TAG = "doubao_duplex";

#define DOUBAO_ENDPOINT          "wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue"
#define DOUBAO_MODEL             "1.2.6.1"
#define DOUBAO_VOICE             "zh_female_xiaohe_jupiter_bigtts"
#define DOUBAO_BUILD_TAG         "V3.1.0-SERVER-VAD-HALF-DUPLEX"
#define DOUBAO_INPUT_FORMAT      "pcm"
#define DOUBAO_OUTPUT_FORMAT     "pcm_s16le"
#define DOUBAO_INPUT_RATE_HZ     16000U
#define DOUBAO_OUTPUT_RATE_HZ    24000U
#define DOUBAO_PACKET_MS         20U
#define DOUBAO_PACKET_SAMPLES    ((DOUBAO_INPUT_RATE_HZ * DOUBAO_PACKET_MS) / 1000U)
#define DOUBAO_PACKET_BYTES      (DOUBAO_PACKET_SAMPLES * sizeof(int16_t))
#define DOUBAO_TX_B64_CAP         ((((DOUBAO_PACKET_BYTES + 2U) / 3U) * 4U) + 4U)
#define DOUBAO_TX_JSON_CAP        (DOUBAO_TX_B64_CAP + 176U)
#define DOUBAO_TX_RING_BYTES     (128U * 1024U)
#define DOUBAO_RX_RING_BYTES     (512U * 1024U)
#define DOUBAO_EVENT_MAX_BYTES   (256U * 1024U)
#define DOUBAO_WS_BUFFER_BYTES     32768
#define DOUBAO_WS_TASK_STACK     4096
#define DOUBAO_WORKER_STACK      6144
#define DOUBAO_WORKER_PRIORITY   5
#define DOUBAO_UPLINK_STACK      5120
#define DOUBAO_UPLINK_PRIORITY   6
#define DOUBAO_CONNECT_TIMEOUT_MS 15000U
#define DOUBAO_SESSION_TIMEOUT_MS 10000U
#define DOUBAO_RESPONSE_START_TIMEOUT_MS 20000U
#define DOUBAO_RESPONSE_IDLE_TIMEOUT_MS  45000U
#define DOUBAO_RESPONSE_TOTAL_TIMEOUT_MS  60000U
#define DOUBAO_RESPONSE_DONE_GRACE_MS      7000U
#define DOUBAO_SEND_TIMEOUT_MS    3000U
#define DOUBAO_PLAY_TIMEOUT_MS    150U
#define DOUBAO_UPLINK_GAIN_X        2U
#define DOUBAO_SERVER_VAD_TAIL_MS 1200U
#define DOUBAO_SERVER_VAD_TAIL_PACKETS \
    ((DOUBAO_SERVER_VAD_TAIL_MS + DOUBAO_PACKET_MS - 1U) / DOUBAO_PACKET_MS)
#define DOUBAO_MAX_TURNS            20U
#define DOUBAO_CONVERSATION_MAX_MS 300000U
#define DOUBAO_TOOL_EVENT_BYTES   (12U * 1024U)
#define DOUBAO_TOOL_OUTPUT_BYTES  (32U * 1024U)
#define DOUBAO_TOOL_REPLY_BYTES   (64U * 1024U)
/*
 * Playback start threshold: 4800 B = 2400 samples @24k = 100 ms of audio, so
 * the speaker never opens on a nearly empty ring (that is heard as a click or
 * as crackling while the network catches up).
 */
#define DOUBAO_PLAY_START_BUFFER_BYTES 4800U
/* Session-long scratch buffer for base64 audio decoding (no per-packet malloc). */
#define DOUBAO_AUDIO_DECODE_BUF   (128U * 1024U)
#define DOUBAO_API_KEY_MAX        256U
#define BIT_WS_CONNECTED   BIT0
#define BIT_SESSION_READY  BIT1
#define BIT_WS_ERROR       BIT2
#define BIT_WORK_KICK      BIT3
#define BIT_SESSION_CLOSED BIT4
#define BIT_UPLINK_STOPPED BIT5
#define BIT_UPLINK_KICK    BIT6

typedef struct {
    bool initialized;
    bool active;
    bool accepting_input;
    bool commit_requested;
    bool commit_sent;
    bool abort_requested;
    bool close_requested;
    bool session_create_sent;
    bool response_started;
    bool response_done;
    bool asr_completed;
    bool asr_text_seen;
    bool audio_started;
    bool audio_done;
    bool playback_started;
    uint32_t audio_delta_events;
    uint32_t audio_delta_b64_bytes;

    /* Raw cloud audio is explicitly requested as PCM16/24k mono LE. Track it
     * before resampling, then track the 16k samples handed to voice_audio so
     * one log separates cloud-format faults from the local resampler/I2S path. */
    uint64_t downlink_raw_abs_sum;
    int64_t downlink_raw_signed_sum;
    uint32_t downlink_raw_samples;
    uint32_t downlink_raw_peak;
    uint32_t downlink_raw_zero_samples;
    uint32_t downlink_raw_clipped_samples;
    uint32_t downlink_raw_bytes;
    uint32_t downlink_diag_chunks;
    uint64_t downlink_resampled_abs_sum;
    int64_t downlink_resampled_signed_sum;
    uint32_t downlink_resampled_samples;
    uint32_t downlink_resampled_peak;
    uint32_t downlink_resampled_zero_samples;
    uint32_t downlink_resampled_clipped_samples;
    uint32_t downlink_play_frames;

    bool callback_sent;
    bool turn_callback_sent;
    bool waiting_followup;
    bool tool_pending;
    bool tool_in_progress;
    bool tool_stage_done_pending;
    bool awaiting_tool_response;
    esp_err_t terminal_status;
    uint32_t turn_index;
    TickType_t conversation_tick;

    esp_websocket_client_handle_t ws;
    TaskHandle_t worker;
    TaskHandle_t uplink_worker;
    EventGroupHandle_t events;

    uint8_t *tx_ring;
    size_t tx_head;
    size_t tx_tail;
    size_t tx_len;

    /* Source-side PCM diagnostics. These are measured before ring buffering. */
    uint64_t capture_abs_sum;
    int64_t capture_signed_sum;
    uint32_t capture_samples;
    uint32_t capture_peak;
    uint32_t capture_zero_samples;
    uint32_t capture_clipped_samples;
    uint32_t capture_push_calls;

    uint8_t *rx_ring;
    size_t rx_head;
    size_t rx_tail;
    size_t rx_len;

    uint8_t *decode_buf;
    char *tx_b64;
    char *tx_json;
    char *tool_event;
    char *tool_output;
    char *tool_reply;

    TickType_t commit_tick;
    TickType_t audio_done_tick;
    TickType_t response_progress_tick;
    uint32_t response_progress_events;

    char *rx_event;
    size_t rx_event_cap;
    size_t rx_event_expected;
    size_t rx_event_received;

    char api_key[DOUBAO_API_KEY_MAX];
    char auth_header[DOUBAO_API_KEY_MAX + 32U];
    char session_id[64];

    voice_duplex_volc_done_fn turn_done;
    voice_duplex_volc_done_fn done;
    void *done_ctx;
} duplex_ctx_t;

static duplex_ctx_t s;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_event_counter;
static StaticTask_t s_uplink_task_tcb;
static StackType_t s_uplink_task_stack[DOUBAO_UPLINK_STACK];

static void mark_response_progress(void);

static size_t ring_write_locked(uint8_t *ring, size_t cap,
                                size_t *head, size_t *len,
                                const uint8_t *src, size_t n)
{
    if (!ring || !src || n == 0 || n > cap - *len) {
        return 0;
    }
    size_t first = cap - *head;
    if (first > n) {
        first = n;
    }
    memcpy(ring + *head, src, first);
    if (n > first) {
        memcpy(ring, src + first, n - first);
    }
    *head = (*head + n) % cap;
    *len += n;
    return n;
}

static size_t ring_read_locked(uint8_t *ring, size_t cap,
                               size_t *tail, size_t *len,
                               uint8_t *dst, size_t n)
{
    if (!ring || !dst || n == 0 || *len == 0) {
        return 0;
    }
    if (n > *len) {
        n = *len;
    }
    size_t first = cap - *tail;
    if (first > n) {
        first = n;
    }
    memcpy(dst, ring + *tail, first);
    if (n > first) {
        memcpy(dst + first, ring, n - first);
    }
    *tail = (*tail + n) % cap;
    *len -= n;
    return n;
}

/* Discard the oldest n bytes without copying them anywhere. */
static size_t ring_drop_locked(uint8_t *ring, size_t cap,
                               size_t *tail, size_t *len, size_t n)
{
    if (!ring || n == 0 || *len == 0) {
        return 0;
    }
    if (n > *len) {
        n = *len;
    }
    *tail = (*tail + n) % cap;
    *len -= n;
    return n;
}

static size_t tx_len_snapshot(void)
{
    size_t n;
    portENTER_CRITICAL(&s_mux);
    n = s.tx_len;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

static size_t rx_len_snapshot(void)
{
    size_t n;
    portENTER_CRITICAL(&s_mux);
    n = s.rx_len;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

static bool state_flag(bool *field)
{
    bool v;
    portENTER_CRITICAL(&s_mux);
    v = *field;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

static void set_terminal_error(esp_err_t err)
{
    portENTER_CRITICAL(&s_mux);
    if (s.terminal_status == ESP_OK) {
        s.terminal_status = err == ESP_OK ? ESP_FAIL : err;
    }
    portEXIT_CRITICAL(&s_mux);
    if (s.events) {
        xEventGroupSetBits(s.events,
                           BIT_WS_ERROR | BIT_WORK_KICK | BIT_UPLINK_KICK);
    }
}

static void new_event_id(char *out, size_t out_size)
{
    uint32_t n;
    portENTER_CRITICAL(&s_mux);
    n = ++s_event_counter;
    portEXIT_CRITICAL(&s_mux);
    snprintf(out, out_size, "espclaw_%08" PRIx32 "_%" PRIu32, esp_random(), n);
}

static esp_err_t ws_send_text(const char *text)
{
    if (!text || !s.ws || !esp_websocket_client_is_connected(s.ws)) {
        return ESP_ERR_INVALID_STATE;
    }
    int len = (int)strlen(text);
    int sent = esp_websocket_client_send_text(s.ws, text, len, pdMS_TO_TICKS(DOUBAO_SEND_TIMEOUT_MS));
    return sent == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_session_create(void)
{
    if (!s.tool_reply) {
        return ESP_ERR_INVALID_STATE;
    }

    char event_id[64];
    new_event_id(event_id, sizeof(event_id));

    int n = snprintf(
        s.tool_reply, DOUBAO_TOOL_REPLY_BYTES,
        "{\"type\":\"session.create\",\"event_id\":\"%s\","
        "\"session\":{\"model\":\"%s\","
        "\"audio\":{"
        "\"input\":{\"format\":{\"type\":\"" DOUBAO_INPUT_FORMAT "\",\"rate\":16000}},"
        "\"output\":{\"format\":{\"type\":\"" DOUBAO_OUTPUT_FORMAT "\",\"rate\":24000},"
        "\"voice\":\"%s\"}}}}",
        event_id, DOUBAO_MODEL, DOUBAO_VOICE);
    if (n <= 0 || n >= (int)DOUBAO_TOOL_REPLY_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = ws_send_text(s.tool_reply);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s.session_create_sent = true;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG,
                 "session.create sent build=%s profile=confirmed-vad-multiturn model=%s input=%s/16k output=%s/24k voice=%s tools=0",
                 DOUBAO_BUILD_TAG, DOUBAO_MODEL, DOUBAO_INPUT_FORMAT,
                 DOUBAO_OUTPUT_FORMAT, DOUBAO_VOICE);
    }
    return err;
}

static esp_err_t send_audio_packet(const uint8_t pcm[DOUBAO_PACKET_BYTES])
{
    if (!s.tx_b64 || !s.tx_json) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t olen = 0;
    int rc = mbedtls_base64_encode((unsigned char *)s.tx_b64, DOUBAO_TX_B64_CAP, &olen,
                                   pcm, DOUBAO_PACKET_BYTES);
    if (rc != 0 || olen + 1U > DOUBAO_TX_B64_CAP) {
        return ESP_FAIL;
    }
    s.tx_b64[olen] = '\0';

    /* The verified V2.6.0 session streamed append frames without event_id.
     * Reuse session-long PSRAM buffers so 50 packets/s do not churn the heap. */
    int n = snprintf(s.tx_json, DOUBAO_TX_JSON_CAP,
                     "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}",
                     s.tx_b64);
    if (n <= 0 || (size_t)n >= DOUBAO_TX_JSON_CAP) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ws_send_text(s.tx_json);
}

static uint32_t apply_uplink_gain(uint8_t pcm[DOUBAO_PACKET_BYTES])
{
    uint32_t clipped = 0;
    for (size_t i = 0; i < DOUBAO_PACKET_BYTES; i += sizeof(int16_t)) {
        int16_t input = (int16_t)((uint16_t)pcm[i] | ((uint16_t)pcm[i + 1U] << 8));
        int32_t output = (int32_t)input * (int32_t)DOUBAO_UPLINK_GAIN_X;
        if (output > INT16_MAX) {
            output = INT16_MAX;
            clipped++;
        } else if (output < INT16_MIN) {
            output = INT16_MIN;
            clipped++;
        }
        pcm[i] = (uint8_t)((uint16_t)output & 0xffU);
        pcm[i + 1U] = (uint8_t)(((uint16_t)output >> 8) & 0xffU);
    }
    return clipped;
}

static esp_err_t send_simple_event(const char *type)
{
    char event_id[64];
    char json[160];
    new_event_id(event_id, sizeof(event_id));
    int n = snprintf(json, sizeof(json),
                     "{\"type\":\"%s\",\"event_id\":\"%s\"}", type, event_id);
    if (n <= 0 || n >= (int)sizeof(json)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = ws_send_text(json);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "send event=%s", json);
    }
    return err;
}

static bool is_weather_tool(const char *name)
{
    static const char *const allowed[] = {
        "weather_get_current",
        "weather_get_hourly",
        "weather_get_daily",
        "weather_get_alerts",
        "weather_get_status",
    };

    if (!name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        if (strcmp(name, allowed[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool json_buf_append(char *out, size_t cap, size_t *io_len,
                            const char *text, size_t text_len)
{
    if (!out || !io_len || !text || *io_len >= cap || text_len >= cap - *io_len) {
        return false;
    }
    memcpy(out + *io_len, text, text_len);
    *io_len += text_len;
    out[*io_len] = '\0';
    return true;
}

static bool json_buf_append_cstr(char *out, size_t cap, size_t *io_len,
                                 const char *text)
{
    return text && json_buf_append(out, cap, io_len, text, strlen(text));
}

static bool json_buf_append_escaped(char *out, size_t cap, size_t *io_len,
                                    const char *text)
{
    static const char hex[] = "0123456789abcdef";
    if (!text) {
        text = "";
    }

    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        char escaped[6];
        const char *chunk = NULL;
        size_t chunk_len = 0;
        switch (*p) {
        case '\"': chunk = "\\\""; chunk_len = 2; break;
        case '\\': chunk = "\\\\"; chunk_len = 2; break;
        case '\b': chunk = "\\b"; chunk_len = 2; break;
        case '\f': chunk = "\\f"; chunk_len = 2; break;
        case '\n': chunk = "\\n"; chunk_len = 2; break;
        case '\r': chunk = "\\r"; chunk_len = 2; break;
        case '\t': chunk = "\\t"; chunk_len = 2; break;
        default:
            if (*p < 0x20U) {
                escaped[0] = '\\';
                escaped[1] = 'u';
                escaped[2] = '0';
                escaped[3] = '0';
                escaped[4] = hex[*p >> 4];
                escaped[5] = hex[*p & 0x0fU];
                chunk = escaped;
                chunk_len = sizeof(escaped);
            } else {
                escaped[0] = (char)*p;
                chunk = escaped;
                chunk_len = 1;
            }
            break;
        }
        if (!json_buf_append(out, cap, io_len, chunk, chunk_len)) {
            return false;
        }
    }
    return true;
}

static esp_err_t queue_tool_event(const char *json)
{
    if (!json || !s.tool_event) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t len = strlen(json);
    if (len + 1U > DOUBAO_TOOL_EVENT_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    portENTER_CRITICAL(&s_mux);
    if (s.tool_pending || s.tool_in_progress) {
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }
    /* Reserve the shared buffer, then copy outside the critical section.
     * tool_event lives in PSRAM and a 12 KiB copy must not mask interrupts. */
    s.tool_in_progress = true;
    portEXIT_CRITICAL(&s_mux);

    memcpy(s.tool_event, json, len + 1U);

    portENTER_CRITICAL(&s_mux);
    s.tool_in_progress = false;
    s.tool_pending = true;
    s.tool_stage_done_pending = true;
    s.response_done = false;
    portEXIT_CRITICAL(&s_mux);

    if (s.events) {
        xEventGroupSetBits(s.events, BIT_WORK_KICK);
    }
    return ESP_OK;
}

static esp_err_t process_pending_tool_calls(void)
{
    portENTER_CRITICAL(&s_mux);
    if (!s.tool_pending || s.tool_in_progress || !s.tool_event ||
        !s.tool_output || !s.tool_reply) {
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }
    s.tool_pending = false;
    s.tool_in_progress = true;
    portEXIT_CRITICAL(&s_mux);

    esp_err_t result = ESP_OK;
    cJSON *event = cJSON_ParseWithLength(s.tool_event, strlen(s.tool_event));
    cJSON *items = event ? cJSON_GetObjectItemCaseSensitive(event, "items") : NULL;
    if (!event || !cJSON_IsArray(items) || cJSON_GetArraySize(items) == 0) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }

    char event_id[64];
    new_event_id(event_id, sizeof(event_id));
    size_t reply_len = 0;
    if (!json_buf_append_cstr(s.tool_reply, DOUBAO_TOOL_REPLY_BYTES, &reply_len,
                              "{\"type\":\"conversation.item.create\",\"event_id\":\"") ||
        !json_buf_append_escaped(s.tool_reply, DOUBAO_TOOL_REPLY_BYTES, &reply_len, event_id) ||
        !json_buf_append_cstr(s.tool_reply, DOUBAO_TOOL_REPLY_BYTES, &reply_len,
                              "\",\"items\":[")) {
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    int item_count = cJSON_GetArraySize(items);
    int emitted = 0;
    for (int i = 0; i < item_count; ++i) {
        cJSON *item = cJSON_GetArrayItem(items, i);
        const char *call_id = cJSON_GetStringValue(
            item ? cJSON_GetObjectItemCaseSensitive(item, "call_id") : NULL);
        const char *name = cJSON_GetStringValue(
            item ? cJSON_GetObjectItemCaseSensitive(item, "name") : NULL);
        const char *arguments = cJSON_GetStringValue(
            item ? cJSON_GetObjectItemCaseSensitive(item, "arguments") : NULL);
        if (!call_id || !call_id[0] || !name || !name[0]) {
            result = ESP_ERR_INVALID_RESPONSE;
            goto done;
        }
        if (!arguments || !arguments[0]) {
            arguments = "{}";
        }

        s.tool_output[0] = '\0';
        esp_err_t call_err = ESP_ERR_NOT_SUPPORTED;
        if (is_weather_tool(name)) {
            claw_cap_call_context_t call_ctx = {
                .session_id = s.session_id,
                .channel = "voice",
                .chat_id = s.session_id,
                .source_cap = "doubao_realtime",
                .correlation_id = call_id,
                .caller = CLAW_CAP_CALLER_SYSTEM,
            };
            ESP_LOGI(TAG, "weather tool start turn=%u name=%s call_id=%s",
                     (unsigned)s.turn_index, name, call_id);
            call_err = claw_cap_call(name, arguments, &call_ctx,
                                     s.tool_output, DOUBAO_TOOL_OUTPUT_BYTES);
        } else {
            snprintf(s.tool_output, DOUBAO_TOOL_OUTPUT_BYTES,
                     "{\"ok\":false,\"error\":\"tool is not allowed on the voice bridge\"}");
        }
        if (!s.tool_output[0]) {
            snprintf(s.tool_output, DOUBAO_TOOL_OUTPUT_BYTES,
                     "{\"ok\":false,\"error\":\"tool call failed\",\"code\":\"%s\"}",
                     esp_err_to_name(call_err));
        }
        ESP_LOGI(TAG,
                 "weather tool done turn=%u name=%s status=%s bytes=%u worker_stack_free=%uB",
                 (unsigned)s.turn_index, name, esp_err_to_name(call_err),
                 (unsigned)strlen(s.tool_output),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));

        if (emitted > 0 &&
            !json_buf_append_cstr(s.tool_reply, DOUBAO_TOOL_REPLY_BYTES, &reply_len, ",")) {
            result = ESP_ERR_INVALID_SIZE;
            goto done;
        }
        if (!json_buf_append_cstr(s.tool_reply, DOUBAO_TOOL_REPLY_BYTES, &reply_len,
                                  "{\"type\":\"message\",\"role\":\"tool\",\"call_id\":\"") ||
            !json_buf_append_escaped(s.tool_reply, DOUBAO_TOOL_REPLY_BYTES, &reply_len, call_id) ||
            !json_buf_append_cstr(s.tool_reply, DOUBAO_TOOL_REPLY_BYTES, &reply_len,
                                  "\",\"content\":[{\"type\":\"input_text\",\"text\":\"") ||
            !json_buf_append_escaped(s.tool_reply, DOUBAO_TOOL_REPLY_BYTES, &reply_len,
                                     s.tool_output) ||
            !json_buf_append_cstr(s.tool_reply, DOUBAO_TOOL_REPLY_BYTES, &reply_len,
                                  "\"}]}")) {
            result = ESP_ERR_INVALID_SIZE;
            goto done;
        }
        emitted++;
    }

    if (!json_buf_append_cstr(s.tool_reply, DOUBAO_TOOL_REPLY_BYTES, &reply_len, "]}")) {
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    /* The function-call stage emits its own response.done. Clear that stage
     * before returning tool results so only the post-tool answer can finish
     * the user turn. Keep tool_in_progress set across the socket write so a
     * late stage response.done cannot win this transition. */
    portENTER_CRITICAL(&s_mux);
    s.response_started = false;
    s.response_done = false;
    s.audio_started = false;
    s.audio_done = false;
    s.awaiting_tool_response = true;
    s.rx_head = s.rx_tail = s.rx_len = 0;
    portEXIT_CRITICAL(&s_mux);

    result = ws_send_text(s.tool_reply);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "tool results returned count=%d bytes=%u",
                 emitted, (unsigned)reply_len);
        mark_response_progress();
    }

done:
    cJSON_Delete(event);
    portENTER_CRITICAL(&s_mux);
    s.tool_event[0] = '\0';
    s.tool_in_progress = false;
    if (result != ESP_OK) {
        s.awaiting_tool_response = false;
    }
    portEXIT_CRITICAL(&s_mux);
    return result;
}

static const char *json_skip_ws(const char *p)
{
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        ++p;
    }
    return p;
}

static const char *json_string_end(const char *p)
{
    bool escaped = false;

    while (p && *p) {
        if (escaped) {
            escaped = false;
        } else if (*p == '\\') {
            escaped = true;
        } else if (*p == '"') {
            return p;
        }
        ++p;
    }
    return NULL;
}

/* Return a string field from the root object without allocating or parsing
 * large base64 audio deltas. Nested items[].type fields are intentionally
 * ignored even if the server changes JSON member ordering. */
static const char *json_string_value(const char *json, const char *key, size_t *out_len)
{
    if (out_len) {
        *out_len = 0;
    }
    if (!json || !key) {
        return NULL;
    }
    size_t wanted_len = strlen(key);
    unsigned object_depth = 0;
    unsigned array_depth = 0;

    for (const char *p = json; *p; ++p) {
        if (*p == '"') {
            const char *start = p + 1;
            const char *end = json_string_end(start);
            if (!end) {
                return NULL;
            }

            if (object_depth == 1U && array_depth == 0U) {
                const char *after_key = json_skip_ws(end + 1);
                size_t candidate_len = (size_t)(end - start);
                if (*after_key == ':' && candidate_len == wanted_len &&
                    memcmp(start, key, wanted_len) == 0) {
                    const char *value_quote = json_skip_ws(after_key + 1);
                    if (*value_quote != '"') {
                        return NULL;
                    }
                    const char *value = value_quote + 1;
                    const char *value_end = json_string_end(value);
                    if (!value_end) {
                        return NULL;
                    }
                    if (out_len) {
                        *out_len = (size_t)(value_end - value);
                    }
                    return value;
                }
            }

            p = end;
            continue;
        }

        if (*p == '{') {
            ++object_depth;
        } else if (*p == '}') {
            if (object_depth == 0U) {
                return NULL;
            }
            --object_depth;
        } else if (*p == '[') {
            ++array_depth;
        } else if (*p == ']') {
            if (array_depth == 0U) {
                return NULL;
            }
            --array_depth;
        }
    }
    return NULL;
}

static bool log_json_text_field(const char *prefix, const char *json, const char *key)
{
    size_t len = 0;
    const char *v = json_string_value(json, key, &len);
    if (v && len > 0) {
        if (len > 320U) len = 320U;
        ESP_LOGI(TAG, "%s: %.*s", prefix, (int)len, v);
        return true;
    }
    return false;
}

static void pcm16_accumulate_bytes_le(const uint8_t *pcm, size_t bytes,
                                      uint64_t *abs_sum, int64_t *signed_sum,
                                      uint32_t *samples, uint32_t *peak,
                                      uint32_t *zeros, uint32_t *clipped)
{
    if (!pcm || !abs_sum || !signed_sum || !samples || !peak || !zeros || !clipped) return;
    size_t usable = bytes & ~(size_t)1U;
    for (size_t i = 0; i < usable; i += 2U) {
        int16_t sample = (int16_t)((uint16_t)pcm[i] | ((uint16_t)pcm[i + 1U] << 8));
        int32_t mag = sample < 0 ? -(int32_t)sample : (int32_t)sample;
        *abs_sum += (uint32_t)mag;
        *signed_sum += sample;
        (*samples)++;
        if ((uint32_t)mag > *peak) *peak = (uint32_t)mag;
        if (sample == 0) (*zeros)++;
        if (sample == INT16_MIN || sample == INT16_MAX) (*clipped)++;
    }
}

static void pcm16_accumulate_samples(const int16_t *pcm, size_t count,
                                     uint64_t *abs_sum, int64_t *signed_sum,
                                     uint32_t *samples, uint32_t *peak,
                                     uint32_t *zeros, uint32_t *clipped)
{
    if (!pcm || !abs_sum || !signed_sum || !samples || !peak || !zeros || !clipped) return;
    for (size_t i = 0; i < count; ++i) {
        int16_t sample = pcm[i];
        int32_t mag = sample < 0 ? -(int32_t)sample : (int32_t)sample;
        *abs_sum += (uint32_t)mag;
        *signed_sum += sample;
        (*samples)++;
        if ((uint32_t)mag > *peak) *peak = (uint32_t)mag;
        if (sample == 0) (*zeros)++;
        if (sample == INT16_MIN || sample == INT16_MAX) (*clipped)++;
    }
}

static void log_downlink_chunk_probe(const uint8_t *pcm, size_t bytes, uint32_t chunk_index)
{
    if (!pcm || bytes == 0 || chunk_index > 1U) return;

    uint64_t le_abs = 0, be_abs = 0;
    int64_t le_sum = 0, be_sum = 0;
    uint32_t le_n = 0, be_n = 0, le_peak = 0, be_peak = 0;
    uint32_t le_zero = 0, be_zero = 0, le_clip = 0, be_clip = 0;
    size_t usable = bytes & ~(size_t)1U;
    for (size_t i = 0; i < usable; i += 2U) {
        int16_t le = (int16_t)((uint16_t)pcm[i] | ((uint16_t)pcm[i + 1U] << 8));
        int16_t be = (int16_t)((uint16_t)pcm[i + 1U] | ((uint16_t)pcm[i] << 8));
        int32_t le_mag = le < 0 ? -(int32_t)le : (int32_t)le;
        int32_t be_mag = be < 0 ? -(int32_t)be : (int32_t)be;
        le_abs += (uint32_t)le_mag; le_sum += le; le_n++;
        be_abs += (uint32_t)be_mag; be_sum += be; be_n++;
        if ((uint32_t)le_mag > le_peak) le_peak = (uint32_t)le_mag;
        if ((uint32_t)be_mag > be_peak) be_peak = (uint32_t)be_mag;
        if (le == 0) le_zero++;
        if (be == 0) be_zero++;
        if (le == INT16_MIN || le == INT16_MAX) le_clip++;
        if (be == INT16_MIN || be == INT16_MAX) be_clip++;
    }

    uint32_t le_mean = le_n ? (uint32_t)(le_abs / le_n) : 0U;
    uint32_t be_mean = be_n ? (uint32_t)(be_abs / be_n) : 0U;
    int32_t le_dc = le_n ? (int32_t)(le_sum / (int64_t)le_n) : 0;
    int32_t be_dc = be_n ? (int32_t)(be_sum / (int64_t)be_n) : 0;

    uint8_t h[16] = {0};
    size_t hn = bytes < sizeof(h) ? bytes : sizeof(h);
    memcpy(h, pcm, hn);
    ESP_LOGI(TAG,
             "downlink chunk#%u format=%s decoded=%u odd=%u head=%02x %02x %02x %02x %02x %02x %02x %02x",
             (unsigned)chunk_index, DOUBAO_OUTPUT_FORMAT,
             (unsigned)bytes, (unsigned)(bytes & 1U),
             h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
    ESP_LOGI(TAG, "downlink PCM16 LE probe mean=%u peak=%u dc=%d zero=%u clip=%u",
             (unsigned)le_mean, (unsigned)le_peak, (int)le_dc,
             (unsigned)le_zero, (unsigned)le_clip);
    ESP_LOGI(TAG, "downlink PCM16 BE probe mean=%u peak=%u dc=%d zero=%u clip=%u",
             (unsigned)be_mean, (unsigned)be_peak, (int)be_dc,
             (unsigned)be_zero, (unsigned)be_clip);

    if (hn >= 4U && pcm[0] == 'R' && pcm[1] == 'I' && pcm[2] == 'F' && pcm[3] == 'F') {
        ESP_LOGE(TAG, "downlink format probe: RIFF/WAV header detected; expected raw PCM16LE");
    } else if (hn >= 4U && pcm[0] == 'O' && pcm[1] == 'g' && pcm[2] == 'g' && pcm[3] == 'S') {
        ESP_LOGE(TAG, "downlink format probe: OggS header detected; server returned OGG instead of raw PCM");
    } else if (hn >= 3U && pcm[0] == 'I' && pcm[1] == 'D' && pcm[2] == '3') {
        ESP_LOGE(TAG, "downlink format probe: ID3 header detected; expected raw PCM16LE");
    }
}

static esp_err_t rx_write_audio_base64(const char *b64, size_t b64_len)
{
    if (!b64 || b64_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t decoded_cap = (b64_len * 3U) / 4U + 8U;
    /*
     * Reuse the session-long decode buffer instead of allocating and freeing
     * PSRAM once per audio packet.  Deltas larger than the scratch buffer fall
     * back to a temporary allocation so no audio is silently truncated.
     */
    bool decoded_is_scratch = false;
    uint8_t *decoded = s.decode_buf;
    if (!decoded || decoded_cap > DOUBAO_AUDIO_DECODE_BUF) {
        decoded = heap_caps_malloc(decoded_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!decoded) {
            return ESP_ERR_NO_MEM;
        }
        decoded_is_scratch = true;
    }
    size_t decoded_len = 0;
    int rc = mbedtls_base64_decode(decoded, decoded_cap, &decoded_len,
                                   (const unsigned char *)b64, b64_len);
    if (rc != 0) {
        if (decoded_is_scratch) free(decoded);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint64_t raw_abs = 0;
    int64_t raw_sum = 0;
    uint32_t raw_samples = 0, raw_peak = 0, raw_zero = 0, raw_clip = 0;
    pcm16_accumulate_bytes_le(decoded, decoded_len,
                              &raw_abs, &raw_sum, &raw_samples, &raw_peak,
                              &raw_zero, &raw_clip);
    uint32_t chunk_index;
    portENTER_CRITICAL(&s_mux);
    s.downlink_diag_chunks++;
    chunk_index = s.downlink_diag_chunks;
    s.downlink_raw_bytes += (uint32_t)decoded_len;
    s.downlink_raw_abs_sum += raw_abs;
    s.downlink_raw_signed_sum += raw_sum;
    s.downlink_raw_samples += raw_samples;
    if (raw_peak > s.downlink_raw_peak) s.downlink_raw_peak = raw_peak;
    s.downlink_raw_zero_samples += raw_zero;
    s.downlink_raw_clipped_samples += raw_clip;
    portEXIT_CRITICAL(&s_mux);
    log_downlink_chunk_probe(decoded, decoded_len, chunk_index);
    if ((decoded_len & 1U) != 0U) {
        ESP_LOGW(TAG, "downlink PCM decoded length is odd: chunk=%u bytes=%u",
                 (unsigned)chunk_index, (unsigned)decoded_len);
    }

    /* A single delta can never be larger than the ring; keep its newest part. */
    const uint8_t *payload = decoded;
    size_t payload_len = decoded_len;
    if (payload_len > DOUBAO_RX_RING_BYTES) {
        payload += payload_len - DOUBAO_RX_RING_BYTES;
        payload_len = DOUBAO_RX_RING_BYTES;
    }

    /*
     * Playback can fall behind when the network or the speaker stalls. Keep the
     * newest audio and drop the oldest instead of failing the write, which
     * would tear the whole duplex session down.
     */
    size_t dropped = 0;
    size_t wrote;
    portENTER_CRITICAL(&s_mux);
    if (payload_len > DOUBAO_RX_RING_BYTES - s.rx_len) {
        size_t need = payload_len - (DOUBAO_RX_RING_BYTES - s.rx_len);
        /* Stay on the 3-sample grouping used by the 24k->16k resampler. */
        need = ((need + 5U) / 6U) * 6U;
        dropped = ring_drop_locked(s.rx_ring, DOUBAO_RX_RING_BYTES,
                                   &s.rx_tail, &s.rx_len, need);
    }
    wrote = ring_write_locked(s.rx_ring, DOUBAO_RX_RING_BYTES,
                              &s.rx_head, &s.rx_len, payload, payload_len);
    portEXIT_CRITICAL(&s_mux);
    if (decoded_is_scratch) free(decoded);
    if (dropped > 0) {
        ESP_LOGW(TAG, "downlink audio ring overflow: dropped %u oldest bytes",
                 (unsigned)dropped);
    }
    if (wrote != payload_len) {
        ESP_LOGE(TAG, "downlink audio ring write short wrote=%u need=%u",
                 (unsigned)wrote, (unsigned)payload_len);
        return ESP_FAIL;
    }
    if (s.events) {
        xEventGroupSetBits(s.events, BIT_WORK_KICK);
    }
    return ESP_OK;
}

static uint32_t ms_since_commit(void)
{
    TickType_t tick;
    portENTER_CRITICAL(&s_mux);
    tick = s.commit_tick;
    portEXIT_CRITICAL(&s_mux);
    if (tick == 0) return 0;
    return (uint32_t)((xTaskGetTickCount() - tick) * portTICK_PERIOD_MS);
}

static void mark_response_progress(void)
{
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&s_mux);
    if (s.commit_sent) {
        s.response_progress_tick = now;
        s.response_progress_events++;
    }
    portEXIT_CRITICAL(&s_mux);
}

static void process_server_event(const char *json)
{
    size_t type_len = 0;
    const char *type = json_string_value(json, "type", &type_len);
    if (!type || type_len == 0) {
        ESP_LOGW(TAG, "server event without type");
        return;
    }

    /*
     * The cloud can spend tens of seconds between accepting the input buffer
     * and producing the transcript. Any post-commit server event proves the
     * request is still moving, so refresh the idle watchdog. A separate total
     * deadline below still bounds a session that only produces keep-alive or
     * otherwise incomplete progress.
     */
    mark_response_progress();

#define TYPE_IS(lit) (type_len == sizeof(lit) - 1U && memcmp(type, lit, sizeof(lit) - 1U) == 0)
    if (TYPE_IS("session.created")) {
        size_t sid_len = 0;
        const char *sid = json_string_value(json, "id", &sid_len);
        if (sid && sid_len > 0) {
            if (sid_len >= sizeof(s.session_id)) sid_len = sizeof(s.session_id) - 1U;
            portENTER_CRITICAL(&s_mux);
            memcpy(s.session_id, sid, sid_len);
            s.session_id[sid_len] = '\0';
            s.session_create_sent = true;
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "session.created server_id=%s", s.session_id);
        } else {
            ESP_LOGI(TAG, "session.created");
            portENTER_CRITICAL(&s_mux);
            s.session_create_sent = true;
            portEXIT_CRITICAL(&s_mux);
        }
        if (s.events) {
            xEventGroupSetBits(s.events,
                               BIT_SESSION_READY | BIT_WORK_KICK | BIT_UPLINK_KICK);
        }
    } else if (TYPE_IS("conversation.item.input_audio_transcription.started")) {
        ESP_LOGI(TAG, "ASR started after_commit=%ums", (unsigned)ms_since_commit());
    } else if (TYPE_IS("conversation.item.input_audio_transcription.delta")) {
        log_json_text_field("ASR partial", json, "delta");
    } else if (TYPE_IS("conversation.item.input_audio_transcription.completed")) {
        ESP_LOGI(TAG, "ASR completed after_commit=%ums", (unsigned)ms_since_commit());
        bool has_text = log_json_text_field("ASR", json, "transcript");
        if (!has_text) {
            /* Current official clients accept text as a compatibility fallback. */
            has_text = log_json_text_field("ASR", json, "text");
        }
        portENTER_CRITICAL(&s_mux);
        s.asr_completed = true;
        s.asr_text_seen = has_text;
        portEXIT_CRITICAL(&s_mux);
        if (!has_text) {
            ESP_LOGE(TAG, "ASR completed without non-empty transcript text");
            set_terminal_error(ESP_ERR_INVALID_RESPONSE);
        }
    } else if (TYPE_IS("conversation.item.input_audio_transcription.failed")) {
        ESP_LOGW(TAG, "ASR failed: %s", json);
        set_terminal_error(ESP_ERR_INVALID_RESPONSE);
    } else if (TYPE_IS("input_audio_buffer.committed")) {
        ESP_LOGI(TAG, "input audio buffer committed after_commit=%ums", (unsigned)ms_since_commit());
    } else if (TYPE_IS("session.closed")) {
        ESP_LOGI(TAG, "session closed");
        if (s.events) xEventGroupSetBits(s.events, BIT_SESSION_CLOSED | BIT_WORK_KICK);
        if (!state_flag(&s.close_requested) && !state_flag(&s.abort_requested)) {
            ESP_LOGW(TAG, "cloud closed the active session unexpectedly");
            set_terminal_error(ESP_ERR_INVALID_STATE);
        }
    } else if (TYPE_IS("response.output_text.delta")) {
        portENTER_CRITICAL(&s_mux);
        s.response_started = true;
        portEXIT_CRITICAL(&s_mux);
    } else if (TYPE_IS("response.output_text.done")) {
        log_json_text_field("assistant text", json, "text");
        portENTER_CRITICAL(&s_mux);
        s.response_started = true;
        portEXIT_CRITICAL(&s_mux);
    } else if (TYPE_IS("response.output_audio.started")) {
        ESP_LOGI(TAG, "output audio started after_commit=%ums ws_stack_hwm=%uB",
                 (unsigned)ms_since_commit(), (unsigned)uxTaskGetStackHighWaterMark(NULL));
        portENTER_CRITICAL(&s_mux);
        s.audio_started = true;
        s.response_started = true;
        portEXIT_CRITICAL(&s_mux);
        /* The speaker is opened by the worker once the pre-roll is buffered. */
    } else if (TYPE_IS("response.output_audio.delta")) {
        size_t delta_len = 0;
        const char *delta = json_string_value(json, "delta", &delta_len);
        if (!delta || delta_len == 0) {
            set_terminal_error(ESP_ERR_INVALID_RESPONSE);
        } else {
            bool first_delta = false;
            portENTER_CRITICAL(&s_mux);
            s.audio_delta_events++;
            first_delta = (s.audio_delta_events == 1U);
            s.audio_delta_b64_bytes += (uint32_t)delta_len;
            portEXIT_CRITICAL(&s_mux);
            if (first_delta) {
                ESP_LOGI(TAG, "first output audio delta b64_bytes=%u ws_stack_hwm=%uB",
                         (unsigned)delta_len, (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
            esp_err_t err = rx_write_audio_base64(delta, delta_len);
            if (err != ESP_OK) set_terminal_error(err);
        }
    } else if (TYPE_IS("response.output_audio.done")) {
        TickType_t audio_done_now = xTaskGetTickCount();
        uint32_t chunks;
        uint32_t b64_bytes;
        portENTER_CRITICAL(&s_mux);
        chunks = s.audio_delta_events;
        b64_bytes = s.audio_delta_b64_bytes;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "output audio done after_commit=%ums chunks=%u b64_bytes=%u",
                 (unsigned)ms_since_commit(), (unsigned)chunks, (unsigned)b64_bytes);
        uint32_t raw_samples, raw_peak, raw_zero, raw_clip, raw_bytes;
        uint64_t raw_abs;
        int64_t raw_sum;
        portENTER_CRITICAL(&s_mux);
        raw_samples = s.downlink_raw_samples;
        raw_peak = s.downlink_raw_peak;
        raw_zero = s.downlink_raw_zero_samples;
        raw_clip = s.downlink_raw_clipped_samples;
        raw_bytes = s.downlink_raw_bytes;
        raw_abs = s.downlink_raw_abs_sum;
        raw_sum = s.downlink_raw_signed_sum;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG,
                 "downlink raw24 PCM16LE summary bytes=%u samples=%u mean_abs=%u peak=%u dc=%d zero_permille=%u clipped=%u",
                 (unsigned)raw_bytes, (unsigned)raw_samples,
                 (unsigned)(raw_samples ? raw_abs / raw_samples : 0U),
                 (unsigned)raw_peak,
                 (int)(raw_samples ? raw_sum / (int64_t)raw_samples : 0),
                 (unsigned)(raw_samples ? ((uint64_t)raw_zero * 1000U) / raw_samples : 0U),
                 (unsigned)raw_clip);
        portENTER_CRITICAL(&s_mux);
        s.audio_done = true;
        s.audio_done_tick = audio_done_now;
        portEXIT_CRITICAL(&s_mux);
        if (s.events) xEventGroupSetBits(s.events, BIT_WORK_KICK);
    } else if (TYPE_IS("response.done")) {
        ESP_LOGI(TAG, "response done after_commit=%ums", (unsigned)ms_since_commit());
        bool intermediate_tool_stage = false;
        portENTER_CRITICAL(&s_mux);
        if (s.tool_stage_done_pending) {
            s.tool_stage_done_pending = false;
            intermediate_tool_stage = true;
        } else {
            intermediate_tool_stage = s.tool_pending || s.tool_in_progress ||
                                      (s.awaiting_tool_response && !s.response_started);
        }
        if (!intermediate_tool_stage) {
            s.response_done = true;
            s.awaiting_tool_response = false;
        }
        portEXIT_CRITICAL(&s_mux);
        if (intermediate_tool_stage) {
            ESP_LOGI(TAG, "function-call stage response.done ignored; awaiting tool answer");
        }
        if (s.events) xEventGroupSetBits(s.events, BIT_WORK_KICK);
    } else if (TYPE_IS("response.function_call_arguments.done")) {
        esp_err_t err = queue_tool_event(json);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "function call queue failed: %s", esp_err_to_name(err));
            set_terminal_error(err);
        } else {
            ESP_LOGI(TAG, "function call queued for ESP-Claw capability worker");
        }
    } else if (TYPE_IS("error")) {
        ESP_LOGE(TAG, "Doubao error: %s", json);
        set_terminal_error(ESP_ERR_INVALID_RESPONSE);
    } else {
        ESP_LOGW(TAG, "unhandled server event type=%.*s json=%s",
                 (int)type_len, type, json);
    }
#undef TYPE_IS
}

static void websocket_event_handler(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "websocket connected");
        if (s.events) {
            xEventGroupSetBits(s.events,
                               BIT_WS_CONNECTED | BIT_WORK_KICK | BIT_UPLINK_KICK);
        }
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        if (s.events) {
            xEventGroupClearBits(s.events, BIT_WS_CONNECTED);
        }
        if (data) {
            ESP_LOGW(TAG, "websocket disconnected close_status=%d", data->close_status_code);
        }
        if (!state_flag(&s.abort_requested) && !state_flag(&s.close_requested)) {
            ESP_LOGW(TAG, "websocket disconnected unexpectedly");
            set_terminal_error(ESP_ERR_INVALID_STATE);
        }
        return;
    }
    if (event_id == WEBSOCKET_EVENT_ERROR) {
        /*
         * The handshake status code is what tells an auth rejection (401/403)
         * apart from a TLS or heap failure.  Without it the real reason for the
         * connection drop is invisible.
         */
        if (data) {
            ESP_LOGE(TAG,
                     "websocket error type=%d handshake_status=%d tls_err=0x%x tls_stack=%d sock_errno=%d ws_stack_hwm=%uB",
                     (int)data->error_handle.error_type,
                     data->error_handle.esp_ws_handshake_status_code,
                     (unsigned)data->error_handle.esp_tls_last_esp_err,
                     data->error_handle.esp_tls_stack_err,
                     data->error_handle.esp_transport_sock_errno,
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        } else {
            ESP_LOGE(TAG, "websocket error");
        }
        set_terminal_error(ESP_FAIL);
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA || !data || !data->data_ptr || data->data_len <= 0) {
        return;
    }

    size_t total = data->payload_len > 0 ? (size_t)data->payload_len : (size_t)data->data_len;
    size_t offset = data->payload_offset >= 0 ? (size_t)data->payload_offset : 0U;
    if (total == 0 || total > DOUBAO_EVENT_MAX_BYTES || offset > total || (size_t)data->data_len > total - offset) {
        ESP_LOGE(TAG, "invalid websocket payload total=%u off=%u chunk=%u",
                 (unsigned)total, (unsigned)offset, (unsigned)data->data_len);
        set_terminal_error(ESP_ERR_INVALID_SIZE);
        return;
    }

    if (offset == 0) {
        free(s.rx_event);
        s.rx_event = heap_caps_malloc(total + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s.rx_event) {
            s.rx_event_cap = 0;
            set_terminal_error(ESP_ERR_NO_MEM);
            return;
        }
        s.rx_event_cap = total + 1U;
        s.rx_event_expected = total;
        s.rx_event_received = 0;
    }
    if (!s.rx_event || s.rx_event_expected != total || offset + (size_t)data->data_len >= s.rx_event_cap) {
        set_terminal_error(ESP_ERR_INVALID_STATE);
        return;
    }

    memcpy(s.rx_event + offset, data->data_ptr, (size_t)data->data_len);
    size_t end = offset + (size_t)data->data_len;
    if (end > s.rx_event_received) s.rx_event_received = end;
    if (s.rx_event_received >= s.rx_event_expected) {
        s.rx_event[s.rx_event_expected] = '\0';
        /* Audio delta JSON contains large base64 payloads. Dumping every delta
         * at INFO level can stall the websocket task on UART and distort the
         * very downlink path being tested. Keep control events visible and
         * account audio chunks separately in process_server_event(). */
        if (strstr(s.rx_event, "\"type\":\"response.output_audio.delta\"") == NULL) {
            ESP_LOGI(TAG, "SERVER EVENT=%s", s.rx_event);
        }
        process_server_event(s.rx_event);
        free(s.rx_event);
        s.rx_event = NULL;
        s.rx_event_cap = 0;
        s.rx_event_expected = 0;
        s.rx_event_received = 0;
    }
}

static size_t tx_pop_packet(uint8_t packet[DOUBAO_PACKET_BYTES], bool allow_partial)
{
    size_t available;
    portENTER_CRITICAL(&s_mux);
    available = s.tx_len;
    portEXIT_CRITICAL(&s_mux);

    if (available < DOUBAO_PACKET_BYTES && !allow_partial) {
        return 0;
    }
    if (available == 0) {
        return 0;
    }
    size_t want = available >= DOUBAO_PACKET_BYTES ? DOUBAO_PACKET_BYTES : available;
    memset(packet, 0, DOUBAO_PACKET_BYTES);
    portENTER_CRITICAL(&s_mux);
    size_t got = ring_read_locked(s.tx_ring, DOUBAO_TX_RING_BYTES,
                                  &s.tx_tail, &s.tx_len, packet, want);
    portEXIT_CRITICAL(&s_mux);
    return got;
}

static size_t rx_pop(uint8_t *dst, size_t max_bytes)
{
    size_t got;
    portENTER_CRITICAL(&s_mux);
    got = ring_read_locked(s.rx_ring, DOUBAO_RX_RING_BYTES,
                           &s.rx_tail, &s.rx_len, dst, max_bytes);
    portEXIT_CRITICAL(&s_mux);
    return got;
}

static size_t resample_24k_to_16k(const uint8_t *src, size_t src_bytes,
                                  int16_t *dst, size_t dst_capacity)
{
    size_t src_samples = src_bytes / sizeof(int16_t);
    src_samples -= src_samples % 3U;
    size_t groups = src_samples / 3U;
    if (groups * 2U > dst_capacity) groups = dst_capacity / 2U;

    size_t out = 0;
    for (size_t g = 0; g < groups; ++g) {
        size_t i = g * 6U;
        /*
         * The session explicitly requests pcm_s16le, so each sample is a
         * signed 16-bit little-endian value. V2.5.6 already ruled out BE.
         */
        int16_t s0 = (int16_t)((uint16_t)src[i] | ((uint16_t)src[i + 1U] << 8));
        int16_t s1 = (int16_t)((uint16_t)src[i + 2U] | ((uint16_t)src[i + 3U] << 8));
        int16_t s2 = (int16_t)((uint16_t)src[i + 4U] | ((uint16_t)src[i + 5U] << 8));
        dst[out++] = s0;
        dst[out++] = (int16_t)(((int32_t)s1 + (int32_t)s2) / 2);
    }
    return out;
}

static void wait_for_uplink_stop(void)
{
    TaskHandle_t task;
    EventGroupHandle_t events;
    portENTER_CRITICAL(&s_mux);
    task = s.uplink_worker;
    events = s.events;
    portEXIT_CRITICAL(&s_mux);
    if (!task || !events) {
        return;
    }

    EventBits_t bits = xEventGroupWaitBits(
        events, BIT_UPLINK_STOPPED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(DOUBAO_SEND_TIMEOUT_MS + 3000U));
    if (!(bits & BIT_UPLINK_STOPPED)) {
        ESP_LOGE(TAG, "uplink task did not stop within shutdown deadline");
    }
}

static void cleanup_session(void)
{
    /* Stop accepting producer writes before any session-owned buffer is freed. */
    portENTER_CRITICAL(&s_mux);
    s.accepting_input = false;
    /* Suppress the expected disconnect callback generated by client_stop(). */
    s.close_requested = true;
    portEXIT_CRITICAL(&s_mux);
    if (s.events) {
        xEventGroupSetBits(s.events, BIT_WORK_KICK | BIT_UPLINK_KICK);
    }

    /* The uplink task owns tx_b64/tx_json and can be inside a bounded WebSocket
     * send. Join it before closing the socket or freeing session buffers. */
    wait_for_uplink_stop();

    if (s.playback_started) {
        (void)voice_audio_playback_stop();
        s.playback_started = false;
    }

    if (s.ws) {
        if (esp_websocket_client_is_connected(s.ws)) {
            if (send_simple_event("session.close") == ESP_OK && s.events) {
                EventBits_t close_bits = xEventGroupWaitBits(
                    s.events, BIT_SESSION_CLOSED | BIT_WS_ERROR,
                    pdFALSE, pdFALSE, pdMS_TO_TICKS(1500));
                if (!(close_bits & BIT_SESSION_CLOSED)) {
                    ESP_LOGW(TAG, "session.close ack timeout; closing websocket anyway");
                }
            }
        }
        (void)esp_websocket_client_stop(s.ws);
        (void)esp_websocket_client_destroy(s.ws);
        s.ws = NULL;
    }

    free(s.rx_event);
    s.rx_event = NULL;
    free(s.decode_buf);
    s.decode_buf = NULL;
    free(s.tx_b64);
    s.tx_b64 = NULL;
    free(s.tx_json);
    s.tx_json = NULL;
    free(s.tool_event);
    s.tool_event = NULL;
    free(s.tool_output);
    s.tool_output = NULL;
    free(s.tool_reply);
    s.tool_reply = NULL;
    free(s.tx_ring);
    s.tx_ring = NULL;
    free(s.rx_ring);
    s.rx_ring = NULL;

    portENTER_CRITICAL(&s_mux);
    s.tx_head = s.tx_tail = s.tx_len = 0;
    s.capture_abs_sum = 0;
    s.capture_signed_sum = 0;
    s.capture_samples = 0;
    s.capture_peak = 0;
    s.capture_zero_samples = 0;
    s.capture_clipped_samples = 0;
    s.capture_push_calls = 0;
    s.commit_tick = 0;
    s.audio_done_tick = 0;
    s.response_progress_tick = 0;
    s.response_progress_events = 0;
    s.rx_head = s.rx_tail = s.rx_len = 0;
    s.active = false;
    s.accepting_input = false;
    s.worker = NULL;
    s.uplink_worker = NULL;
    /* A new conversation must start from a clean protocol state. */
    s.commit_requested = false;
    s.commit_sent = false;
    s.abort_requested = false;
    s.close_requested = false;
    s.session_create_sent = false;
    s.response_started = false;
    s.response_done = false;
    s.asr_completed = false;
    s.asr_text_seen = false;
    s.audio_started = false;
    s.audio_done = false;
    s.playback_started = false;
    s.turn_callback_sent = false;
    s.waiting_followup = false;
    s.tool_pending = false;
    s.tool_in_progress = false;
    s.tool_stage_done_pending = false;
    s.awaiting_tool_response = false;
    s.turn_index = 0;
    s.conversation_tick = 0;
    s.audio_delta_events = 0;
    s.audio_delta_b64_bytes = 0;
    s.downlink_raw_abs_sum = 0;
    s.downlink_raw_signed_sum = 0;
    s.downlink_raw_samples = 0;
    s.downlink_raw_peak = 0;
    s.downlink_raw_zero_samples = 0;
    s.downlink_raw_clipped_samples = 0;
    s.downlink_raw_bytes = 0;
    s.downlink_diag_chunks = 0;
    s.downlink_resampled_abs_sum = 0;
    s.downlink_resampled_signed_sum = 0;
    s.downlink_resampled_samples = 0;
    s.downlink_resampled_peak = 0;
    s.downlink_resampled_zero_samples = 0;
    s.downlink_resampled_clipped_samples = 0;
    s.downlink_play_frames = 0;
    portEXIT_CRITICAL(&s_mux);
}

static void notify_done_once(esp_err_t status)
{
    voice_duplex_volc_done_fn cb = NULL;
    void *ctx = NULL;
    portENTER_CRITICAL(&s_mux);
    if (!s.callback_sent) {
        s.callback_sent = true;
        cb = s.done;
        ctx = s.done_ctx;
    }
    portEXIT_CRITICAL(&s_mux);
    if (cb) cb(status, ctx);
}

static void notify_turn_done_once(esp_err_t status)
{
    voice_duplex_volc_done_fn cb = NULL;
    void *ctx = NULL;
    portENTER_CRITICAL(&s_mux);
    if (!s.turn_callback_sent) {
        s.turn_callback_sent = true;
        cb = s.turn_done;
        ctx = s.done_ctx;
    }
    portEXIT_CRITICAL(&s_mux);
    if (cb) {
        cb(status, ctx);
    }
}

static void duplex_uplink_worker(void *arg)
{
    (void)arg;
    esp_err_t status = ESP_OK;
    TickType_t next_audio_tick = 0;
    TickType_t first_tx_tick = 0;
    TickType_t last_tx_tick = 0;
    uint32_t tx_send_sum_ms = 0;
    uint32_t tx_send_count = 0;
    uint32_t tx_send_min_ms = UINT32_MAX;
    uint32_t tx_send_max_ms = 0;
    uint32_t tx_packets = 0;
    uint32_t tx_source_bytes = 0;
    uint32_t tx_wire_bytes = 0;
    uint32_t tx_interval_sum_ms = 0;
    uint32_t tx_interval_count = 0;
    uint32_t tx_interval_min_ms = UINT32_MAX;
    uint32_t tx_interval_max_ms = 0;
    uint64_t tx_abs_sum = 0;
    uint32_t tx_samples = 0;
    uint32_t tx_peak = 0;
    uint32_t tx_gain_clipped = 0;
    uint32_t tx_tail_silence_packets = 0;
    uint32_t local_turn_index = 0;
    uint8_t packet[DOUBAO_PACKET_BYTES];

    ESP_LOGI(TAG,
             "half-duplex uplink task ready packet_ms=%u packet_bytes=%u ring=%u stack=%u priority=%u",
             (unsigned)DOUBAO_PACKET_MS, (unsigned)DOUBAO_PACKET_BYTES,
             (unsigned)DOUBAO_TX_RING_BYTES, (unsigned)DOUBAO_UPLINK_STACK,
             (unsigned)DOUBAO_UPLINK_PRIORITY);

    for (;;) {
        bool abort_requested;
        bool close_requested;
        esp_err_t terminal;
        portENTER_CRITICAL(&s_mux);
        abort_requested = s.abort_requested;
        close_requested = s.close_requested;
        terminal = s.terminal_status;
        portEXIT_CRITICAL(&s_mux);
        if (abort_requested || close_requested || terminal != ESP_OK) {
            break;
        }

        EventBits_t bits = s.events ? xEventGroupGetBits(s.events) : 0;
        if ((bits & (BIT_WS_CONNECTED | BIT_SESSION_READY)) !=
            (BIT_WS_CONNECTED | BIT_SESSION_READY)) {
            if (s.events) {
                xEventGroupWaitBits(s.events,
                                    BIT_SESSION_READY | BIT_WS_ERROR,
                                    pdFALSE, pdFALSE, pdMS_TO_TICKS(DOUBAO_PACKET_MS));
            } else {
                vTaskDelay(pdMS_TO_TICKS(DOUBAO_PACKET_MS));
            }
            continue;
        }

        uint32_t current_turn_index;
        bool commit_requested;
        bool commit_sent;
        portENTER_CRITICAL(&s_mux);
        current_turn_index = s.turn_index;
        commit_requested = s.commit_requested;
        commit_sent = s.commit_sent;
        portEXIT_CRITICAL(&s_mux);

        if (current_turn_index != local_turn_index) {
            local_turn_index = current_turn_index;
            next_audio_tick = 0;
            first_tx_tick = 0;
            last_tx_tick = 0;
            tx_send_sum_ms = 0;
            tx_send_count = 0;
            tx_send_min_ms = UINT32_MAX;
            tx_send_max_ms = 0;
            tx_packets = 0;
            tx_source_bytes = 0;
            tx_wire_bytes = 0;
            tx_interval_sum_ms = 0;
            tx_interval_count = 0;
            tx_interval_min_ms = UINT32_MAX;
            tx_interval_max_ms = 0;
            tx_abs_sum = 0;
            tx_samples = 0;
            tx_peak = 0;
            tx_gain_clipped = 0;
            tx_tail_silence_packets = 0;
            ESP_LOGI(TAG, "half-duplex uplink switched to turn=%u",
                     (unsigned)local_turn_index);
        }

        size_t queued = tx_len_snapshot();
        bool server_finalized;
        portENTER_CRITICAL(&s_mux);
        server_finalized = s.asr_completed || s.response_started || s.audio_started;
        portEXIT_CRITICAL(&s_mux);
        if (commit_requested && !commit_sent && server_finalized && queued > 0) {
            size_t discarded;
            portENTER_CRITICAL(&s_mux);
            discarded = s.tx_len;
            s.tx_tail = s.tx_head;
            s.tx_len = 0;
            portEXIT_CRITICAL(&s_mux);
            queued = 0;
            ESP_LOGI(TAG,
                     "server finalized turn; discarded %u queued tail bytes before response playback",
                     (unsigned)discarded);
        }
        bool packet_ready = queued >= DOUBAO_PACKET_BYTES ||
                            (commit_requested && queued > 0);
        bool commit_ready = commit_requested && !commit_sent && queued == 0;
        bool tail_silence_ready = commit_ready && !server_finalized &&
                                  tx_tail_silence_packets <
                                      DOUBAO_SERVER_VAD_TAIL_PACKETS;
        if (commit_sent || (!packet_ready && !commit_ready && !tail_silence_ready)) {
            if (s.events) {
                xEventGroupWaitBits(s.events, BIT_UPLINK_KICK | BIT_WS_ERROR,
                                    pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            continue;
        }

        size_t got = 0;
        bool sent_tail_silence = false;
        if (packet_ready || tail_silence_ready) {
            TickType_t period_ticks = pdMS_TO_TICKS(DOUBAO_PACKET_MS);
            if (period_ticks == 0) {
                period_ticks = 1;
            }
            TickType_t now = xTaskGetTickCount();
            if (next_audio_tick == 0) {
                next_audio_tick = now;
            }
            if ((int32_t)(next_audio_tick - now) > 0) {
                vTaskDelay(next_audio_tick - now);
            }

            if (packet_ready) {
                got = tx_pop_packet(packet, commit_requested);
                if (got == 0) {
                    continue;
                }
                tx_gain_clipped += apply_uplink_gain(packet);
            } else {
                memset(packet, 0, sizeof(packet));
                sent_tail_silence = true;
            }

            TickType_t tx_tick = xTaskGetTickCount();
            TickType_t send_begin_tick = tx_tick;
            status = send_audio_packet(packet);
            TickType_t send_end_tick = xTaskGetTickCount();
            uint32_t send_ms =
                (uint32_t)((send_end_tick - send_begin_tick) * portTICK_PERIOD_MS);
            if (status != ESP_OK) {
                ESP_LOGE(TAG, "half-duplex uplink send failed: %s",
                         esp_err_to_name(status));
                break;
            }

            tx_send_sum_ms += send_ms;
            tx_send_count++;
            if (send_ms < tx_send_min_ms) tx_send_min_ms = send_ms;
            if (send_ms > tx_send_max_ms) tx_send_max_ms = send_ms;

            if (first_tx_tick == 0) first_tx_tick = tx_tick;
            if (last_tx_tick != 0) {
                uint32_t interval_ms =
                    (uint32_t)((tx_tick - last_tx_tick) * portTICK_PERIOD_MS);
                tx_interval_sum_ms += interval_ms;
                tx_interval_count++;
                if (interval_ms < tx_interval_min_ms) tx_interval_min_ms = interval_ms;
                if (interval_ms > tx_interval_max_ms) tx_interval_max_ms = interval_ms;
            }
            last_tx_tick = tx_tick;
            tx_packets++;
            tx_source_bytes += (uint32_t)got;
            tx_wire_bytes += DOUBAO_PACKET_BYTES;
            if (sent_tail_silence) {
                tx_tail_silence_packets++;
            } else {
                for (size_t i = 0; i < DOUBAO_PACKET_SAMPLES; ++i) {
                    size_t off = i * sizeof(int16_t);
                    int16_t sample = (int16_t)((uint16_t)packet[off] |
                                               ((uint16_t)packet[off + 1U] << 8));
                    int32_t mag = sample < 0 ? -(int32_t)sample : (int32_t)sample;
                    tx_abs_sum += (uint32_t)mag;
                    tx_samples++;
                    if ((uint32_t)mag > tx_peak) tx_peak = (uint32_t)mag;
                }
            }

            next_audio_tick += period_ticks;
            TickType_t after_send_tick = xTaskGetTickCount();
            if ((int32_t)(after_send_tick - next_audio_tick) > (int32_t)period_ticks) {
                /* Do not burst after a network stall. Resume from the current
                 * time and preserve the 20 ms packet cadence. */
                next_audio_tick = after_send_tick;
            }
            if (sent_tail_silence) {
                if (tx_tail_silence_packets == 1U) {
                    ESP_LOGI(TAG,
                             "microphone drained; sending bounded server-VAD silence tail=%ums",
                             (unsigned)DOUBAO_SERVER_VAD_TAIL_MS);
                }
                continue;
            }
        }

        portENTER_CRITICAL(&s_mux);
        commit_requested = s.commit_requested;
        commit_sent = s.commit_sent;
        current_turn_index = s.turn_index;
        portEXIT_CRITICAL(&s_mux);
        if (current_turn_index == local_turn_index && commit_requested &&
            !commit_sent && tx_len_snapshot() == 0) {
            if (tx_packets == 0 && tx_source_bytes == 0) {
                ESP_LOGE(TAG, "MIC-ASR turn rejected: no microphone PCM was sent");
                status = ESP_ERR_INVALID_SIZE;
                break;
            }

            uint32_t span_ms = 0;
            if (first_tx_tick != 0 && last_tx_tick != 0) {
                span_ms =
                    (uint32_t)((last_tx_tick - first_tx_tick) * portTICK_PERIOD_MS);
            }
            uint32_t avg_interval_ms = tx_interval_count ?
                (tx_interval_sum_ms / tx_interval_count) : 0U;
            uint32_t min_interval_ms = tx_interval_count ? tx_interval_min_ms : 0U;
            uint32_t mean_abs = tx_samples ? (uint32_t)(tx_abs_sum / tx_samples) : 0U;
            uint32_t expected_span_ms =
                tx_packets > 0 ? (tx_packets - 1U) * DOUBAO_PACKET_MS : 0U;
            int32_t drift_ms = (int32_t)span_ms - (int32_t)expected_span_ms;
            ESP_LOGI(TAG,
                     "MIC-ASR uplink packet_ms=%u packets=%u source_bytes=%u wire_bytes=%u source_ms=%u wire_ms=%u server_vad_tail_packets=%u span_ms=%u expected_span_ms=%u drift_ms=%d interval_ms[min/avg/max]=%u/%u/%u send_call_ms[min/avg/max]=%u/%u/%u gain=%ux gain_clipped=%u mean_abs=%u peak=%u",
                     (unsigned)DOUBAO_PACKET_MS, (unsigned)tx_packets,
                     (unsigned)tx_source_bytes, (unsigned)tx_wire_bytes,
                     (unsigned)(tx_source_bytes * 1000U /
                                (DOUBAO_INPUT_RATE_HZ * sizeof(int16_t))),
                     (unsigned)(tx_wire_bytes * 1000U /
                                (DOUBAO_INPUT_RATE_HZ * sizeof(int16_t))),
                     (unsigned)tx_tail_silence_packets,
                     (unsigned)span_ms, (unsigned)expected_span_ms, (int)drift_ms,
                     (unsigned)min_interval_ms, (unsigned)avg_interval_ms,
                     (unsigned)tx_interval_max_ms,
                     (unsigned)(tx_send_count ? tx_send_min_ms : 0U),
                     (unsigned)(tx_send_count ? tx_send_sum_ms / tx_send_count : 0U),
                     (unsigned)tx_send_max_ms, (unsigned)DOUBAO_UPLINK_GAIN_X,
                     (unsigned)tx_gain_clipped, (unsigned)mean_abs,
                     (unsigned)tx_peak);

            portENTER_CRITICAL(&s_mux);
            server_finalized = s.asr_completed || s.response_started || s.audio_started;
            portEXIT_CRITICAL(&s_mux);
            if (!server_finalized) {
                status = send_simple_event("input_audio_buffer.commit");
                if (status != ESP_OK) {
                    ESP_LOGE(TAG, "input_audio_buffer.commit failed: %s",
                             esp_err_to_name(status));
                    break;
                }
                ESP_LOGW(TAG,
                         "server VAD did not finalize after %ums zero tail; explicit commit fallback sent",
                         (unsigned)DOUBAO_SERVER_VAD_TAIL_MS);
            } else {
                ESP_LOGI(TAG,
                         "server finalized turn during zero tail; explicit commit skipped");
            }

            TickType_t committed_now = xTaskGetTickCount();
            portENTER_CRITICAL(&s_mux);
            if (s.turn_index == local_turn_index && s.commit_requested &&
                !s.commit_sent && s.tx_len == 0) {
                s.commit_sent = true;
                s.commit_tick = committed_now;
                s.response_progress_tick = committed_now;
                s.response_progress_events = 0;
            }
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG,
                     "MIC-ASR uplink paused until follow-up turn server_finalized=%d",
                     server_finalized ? 1 : 0);
        }
    }

    bool expected_stop;
    portENTER_CRITICAL(&s_mux);
    expected_stop = s.abort_requested || s.close_requested || s.terminal_status != ESP_OK;
    s.uplink_worker = NULL;
    portEXIT_CRITICAL(&s_mux);
    if (status != ESP_OK && !expected_stop) {
        set_terminal_error(status);
    }
    ESP_LOGI(TAG, "half-duplex uplink task stopped status=%s stack_free=%uB",
             esp_err_to_name(status),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (s.events) {
        xEventGroupSetBits(s.events, BIT_UPLINK_STOPPED | BIT_WORK_KICK);
    }
    vTaskDelete(NULL);
}

static void duplex_worker(void *arg)
{
    (void)arg;
    esp_err_t status = ESP_OK;
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t session_tick = 0;
    uint32_t local_turn_index = 1;
    uint8_t rx24[960]; /* 20 ms of signed 24 kHz mono PCM */
    int16_t rx16[320];

    ESP_LOGI(TAG, "begin realtime session id=%s", s.session_id);

    for (;;) {
        bool abort_requested = state_flag(&s.abort_requested);
        bool close_requested = state_flag(&s.close_requested);
        esp_err_t terminal;
        portENTER_CRITICAL(&s_mux);
        terminal = s.terminal_status;
        portEXIT_CRITICAL(&s_mux);
        if (abort_requested) {
            status = ESP_ERR_INVALID_STATE;
            break;
        }
        if (close_requested) {
            status = ESP_OK;
            break;
        }
        if (terminal != ESP_OK) {
            status = terminal;
            break;
        }

        EventBits_t bits = s.events ? xEventGroupGetBits(s.events) : 0;
        TickType_t now = xTaskGetTickCount();

        if (!(bits & BIT_WS_CONNECTED)) {
            if ((now - start_tick) * portTICK_PERIOD_MS > DOUBAO_CONNECT_TIMEOUT_MS) {
                ESP_LOGE(TAG, "websocket connect timeout");
                status = ESP_ERR_TIMEOUT;
                break;
            }
            if (s.events) xEventGroupWaitBits(s.events, BIT_WS_CONNECTED | BIT_WS_ERROR,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(50));
            continue;
        }

        if (!s.session_create_sent) {
            status = send_session_create();
            if (status != ESP_OK) break;
            session_tick = now;
        }

        bits = s.events ? xEventGroupGetBits(s.events) : 0;
        if (!(bits & BIT_SESSION_READY)) {
            if (session_tick != 0 && (now - session_tick) * portTICK_PERIOD_MS > DOUBAO_SESSION_TIMEOUT_MS) {
                ESP_LOGE(TAG, "session.created timeout");
                status = ESP_ERR_TIMEOUT;
                break;
            }
            if (s.events) xEventGroupWaitBits(s.events, BIT_SESSION_READY | BIT_WS_ERROR,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(20));
            continue;
        }

        portENTER_CRITICAL(&s_mux);
        if (s.conversation_tick == 0) {
            s.conversation_tick = now;
        }
        uint32_t current_turn_index = s.turn_index;
        bool waiting_followup = s.waiting_followup;
        portEXIT_CRITICAL(&s_mux);

        if (waiting_followup) {
            if (s.events) {
                xEventGroupWaitBits(s.events, BIT_WORK_KICK | BIT_WS_ERROR,
                                    pdTRUE, pdFALSE, pdMS_TO_TICKS(50));
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            continue;
        }

        if (current_turn_index != local_turn_index) {
            local_turn_index = current_turn_index;
            ESP_LOGI(TAG, "follow-up turn=%u ready on existing cloud session",
                     (unsigned)local_turn_index);
        }

        bool commit_sent = state_flag(&s.commit_sent);
        bool server_finished = state_flag(&s.response_done) || state_flag(&s.audio_done);
        size_t rx_ready = rx_len_snapshot();
        /*
         * Buffer a short pre-roll before opening the speaker so ordinary
         * network jitter does not immediately underrun the 12 ms DMA ring.
         * Once playing, drain down to the last sample - and if the server has
         * already finished, start right away so short replies are not stuck.
         */
        size_t rx_trigger = (s.playback_started || server_finished) ?
                            6U : DOUBAO_PLAY_START_BUFFER_BYTES;
        if (commit_sent && rx_ready >= rx_trigger) {
            if (!s.playback_started) {
                status = voice_audio_playback_start();
                if (status != ESP_OK) {
                    ESP_LOGE(TAG, "playback start failed: %s", esp_err_to_name(status));
                    break;
                }
                s.playback_started = true;
                ESP_LOGI(TAG,
                         "downlink playback started format=%s 24k->16k endian=LE pacing=continuous prefill=%u bytes",
                         DOUBAO_OUTPUT_FORMAT, (unsigned)rx_ready);
            }

            size_t avail = rx_len_snapshot();
            size_t take = avail >= sizeof(rx24) ? sizeof(rx24) : avail;
            take -= take % 6U;
            if (take > 0) {
                size_t got = rx_pop(rx24, take);
                size_t out_samples = resample_24k_to_16k(rx24, got, rx16, sizeof(rx16) / sizeof(rx16[0]));
                if (out_samples > 0) {
                    uint64_t frame_abs = 0;
                    int64_t frame_sum = 0;
                    uint32_t frame_n = 0, frame_peak = 0, frame_zero = 0, frame_clip = 0;
                    pcm16_accumulate_samples(rx16, out_samples,
                                             &frame_abs, &frame_sum, &frame_n, &frame_peak,
                                             &frame_zero, &frame_clip);
                    uint32_t play_frame;
                    portENTER_CRITICAL(&s_mux);
                    s.downlink_play_frames++;
                    play_frame = s.downlink_play_frames;
                    s.downlink_resampled_abs_sum += frame_abs;
                    s.downlink_resampled_signed_sum += frame_sum;
                    s.downlink_resampled_samples += frame_n;
                    if (frame_peak > s.downlink_resampled_peak) s.downlink_resampled_peak = frame_peak;
                    s.downlink_resampled_zero_samples += frame_zero;
                    s.downlink_resampled_clipped_samples += frame_clip;
                    portEXIT_CRITICAL(&s_mux);
                    if (play_frame == 1U) {
                        ESP_LOGI(TAG,
                                 "downlink resample frame#%u src24_bytes=%u out16_samples=%u mean_abs=%u peak=%u dc=%d zero=%u clip=%u first=%d,%d,%d,%d",
                                 (unsigned)play_frame, (unsigned)got, (unsigned)out_samples,
                                 (unsigned)(frame_n ? frame_abs / frame_n : 0U),
                                 (unsigned)frame_peak,
                                 (int)(frame_n ? frame_sum / (int64_t)frame_n : 0),
                                 (unsigned)frame_zero, (unsigned)frame_clip,
                                 (int)(out_samples > 0 ? rx16[0] : 0),
                                 (int)(out_samples > 1 ? rx16[1] : 0),
                                 (int)(out_samples > 2 ? rx16[2] : 0),
                                 (int)(out_samples > 3 ? rx16[3] : 0));
                    }
                    TickType_t play_write_t0 = xTaskGetTickCount();
                    status = voice_audio_playback_write(rx16, out_samples, DOUBAO_PLAY_TIMEOUT_MS);
                    TickType_t play_write_t1 = xTaskGetTickCount();
                    if (status != ESP_OK) {
                        ESP_LOGE(TAG, "playback write failed: %s", esp_err_to_name(status));
                        break;
                    }
                    if (play_frame == 1U) {
                        ESP_LOGI(TAG,
                                 "downlink pacing fix active: first 20ms PCM write took %u ms; queued audio drains without extra 10ms worker sleep",
                                 (unsigned)((play_write_t1 - play_write_t0) * portTICK_PERIOD_MS));
                    }

                    /*
                     * CRITICAL V2.5.8 FIX:
                     * voice_audio_playback_write() already paces this 320-sample
                     * frame at the physical 16 kHz I2S rate.  Falling through to
                     * the generic 10 ms worker wait after every 20 ms audio frame
                     * stretched playback by ~1.42x in the V2.5.6 logs and can
                     * repeatedly underrun the tiny I2S DMA ring, producing the
                     * user's loud buzz/noise.  If more PCM is queued, loop
                     * immediately and let the blocking I2S write be the clock.
                     */
                    if (rx_len_snapshot() >= 6U) {
                        continue;
                    }
                }
            }
        }

        if (state_flag(&s.tool_pending)) {
            status = process_pending_tool_calls();
            if (status != ESP_OK) {
                ESP_LOGE(TAG, "ESP-Claw tool bridge failed: %s", esp_err_to_name(status));
                break;
            }
            continue;
        }

        bool response_done = state_flag(&s.response_done);
        bool audio_done = state_flag(&s.audio_done);
        bool audio_started = state_flag(&s.audio_started);
        TickType_t audio_done_at;
        bool fallback_allowed;
        portENTER_CRITICAL(&s_mux);
        audio_done_at = s.audio_done_tick;
        fallback_allowed = s.asr_completed && s.asr_text_seen &&
                           !s.tool_pending && !s.tool_in_progress &&
                           !s.tool_stage_done_pending && !s.awaiting_tool_response;
        portEXIT_CRITICAL(&s_mux);
        bool response_done_fallback = !response_done && audio_started && audio_done &&
                                      fallback_allowed && audio_done_at != 0 &&
                                      (xTaskGetTickCount() - audio_done_at) >=
                                          pdMS_TO_TICKS(DOUBAO_RESPONSE_DONE_GRACE_MS);
        if ((response_done || response_done_fallback) && rx_len_snapshot() < 6U &&
            (audio_done || !audio_started)) {
            uint32_t rs_samples, rs_peak, rs_zero, rs_clip, rs_frames;
            uint64_t rs_abs;
            int64_t rs_sum;
            bool asr_completed;
            bool asr_text_seen;
            portENTER_CRITICAL(&s_mux);
            rs_samples = s.downlink_resampled_samples;
            rs_peak = s.downlink_resampled_peak;
            rs_zero = s.downlink_resampled_zero_samples;
            rs_clip = s.downlink_resampled_clipped_samples;
            rs_frames = s.downlink_play_frames;
            rs_abs = s.downlink_resampled_abs_sum;
            rs_sum = s.downlink_resampled_signed_sum;
            asr_completed = s.asr_completed;
            asr_text_seen = s.asr_text_seen;
            portEXIT_CRITICAL(&s_mux);
            if (response_done_fallback) {
                ESP_LOGW(TAG,
                         "response.done missing %ums after complete audio; accepting validated turn",
                         (unsigned)DOUBAO_RESPONSE_DONE_GRACE_MS);
            }
            ESP_LOGI(TAG,
                     "downlink resampled16 summary frames=%u samples=%u mean_abs=%u peak=%u dc=%d zero_permille=%u clipped=%u",
                     (unsigned)rs_frames, (unsigned)rs_samples,
                     (unsigned)(rs_samples ? rs_abs / rs_samples : 0U),
                     (unsigned)rs_peak,
                      (int)(rs_samples ? rs_sum / (int64_t)rs_samples : 0),
                      (unsigned)(rs_samples ? ((uint64_t)rs_zero * 1000U) / rs_samples : 0U),
                      (unsigned)rs_clip);
            if (!asr_completed || !asr_text_seen) {
                ESP_LOGE(TAG,
                         "MIC-ASR validation failed: completed=%d nonempty_text=%d",
                         asr_completed ? 1 : 0, asr_text_seen ? 1 : 0);
                status = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            if (!audio_started || !audio_done || rs_samples == 0) {
                ESP_LOGE(TAG,
                         "MIC-ASR validation failed: audio_started=%d audio_done=%d played_samples=%u",
                         audio_started ? 1 : 0, audio_done ? 1 : 0,
                         (unsigned)rs_samples);
                status = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            ESP_LOGI(TAG, "Duplex turn=%u response complete", (unsigned)local_turn_index);
            /*
             * i2s_channel_write() only queues into the DMA ring, so let the
             * last few milliseconds reach the codec before cleanup_session()
             * disables the TX channel.
             */
            vTaskDelay(pdMS_TO_TICKS(50));
            if (s.playback_started) {
                status = voice_audio_playback_stop();
                if (status != ESP_OK) {
                    ESP_LOGE(TAG, "playback stop after turn failed: %s",
                             esp_err_to_name(status));
                    break;
                }
                portENTER_CRITICAL(&s_mux);
                s.playback_started = false;
                portEXIT_CRITICAL(&s_mux);
            }

            TickType_t conversation_tick;
            portENTER_CRITICAL(&s_mux);
            conversation_tick = s.conversation_tick;
            s.accepting_input = false;
            s.waiting_followup = true;
            portEXIT_CRITICAL(&s_mux);

            uint32_t conversation_ms = conversation_tick == 0 ? 0U :
                (uint32_t)((xTaskGetTickCount() - conversation_tick) * portTICK_PERIOD_MS);
            if (local_turn_index >= DOUBAO_MAX_TURNS ||
                conversation_ms >= DOUBAO_CONVERSATION_MAX_MS) {
                ESP_LOGI(TAG,
                         "conversation limit reached turns=%u elapsed=%ums; closing session",
                         (unsigned)local_turn_index, (unsigned)conversation_ms);
                status = ESP_OK;
                break;
            }

            ESP_LOGI(TAG,
                     "turn=%u complete; keeping session open for follow-up turns_left=%u",
                     (unsigned)local_turn_index,
                     (unsigned)(DOUBAO_MAX_TURNS - local_turn_index));
            notify_turn_done_once(ESP_OK);
            continue;
        }

        /* Use a fresh tick after send/playback calls that may block. */
        TickType_t response_now = xTaskGetTickCount();
        TickType_t committed_at;
        TickType_t progressed_at;
        uint32_t progress_events;
        portENTER_CRITICAL(&s_mux);
        committed_at = s.commit_tick;
        progressed_at = s.response_progress_tick;
        progress_events = s.response_progress_events;
        portEXIT_CRITICAL(&s_mux);
        if (commit_sent && committed_at != 0) {
            if (progressed_at == 0) {
                progressed_at = committed_at;
            }
            uint32_t total_ms = (uint32_t)((response_now - committed_at) * portTICK_PERIOD_MS);
            uint32_t idle_ms = (uint32_t)((response_now - progressed_at) * portTICK_PERIOD_MS);
            bool response_started = state_flag(&s.response_started) ||
                                    state_flag(&s.audio_started) ||
                                    state_flag(&s.asr_completed);
            if (!response_started &&
                (response_now - committed_at) >
                    pdMS_TO_TICKS(DOUBAO_RESPONSE_START_TIMEOUT_MS)) {
                ESP_LOGE(TAG,
                         "response start timeout total=%u ms progress_events=%u",
                         (unsigned)total_ms, (unsigned)progress_events);
                status = ESP_ERR_TIMEOUT;
                break;
            }
            if ((response_now - committed_at) > pdMS_TO_TICKS(DOUBAO_RESPONSE_TOTAL_TIMEOUT_MS)) {
                ESP_LOGE(TAG,
                         "response total timeout total=%u ms idle=%u ms progress_events=%u",
                         (unsigned)total_ms, (unsigned)idle_ms, (unsigned)progress_events);
                status = ESP_ERR_TIMEOUT;
                break;
            }
            if ((response_now - progressed_at) > pdMS_TO_TICKS(DOUBAO_RESPONSE_IDLE_TIMEOUT_MS)) {
                ESP_LOGE(TAG,
                         "response idle timeout total=%u ms idle=%u ms progress_events=%u",
                         (unsigned)total_ms, (unsigned)idle_ms, (unsigned)progress_events);
                status = ESP_ERR_TIMEOUT;
                break;
            }
        }

        if (s.events) xEventGroupWaitBits(s.events, BIT_WORK_KICK | BIT_WS_ERROR,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(10));
        else vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (status != ESP_OK) {
        set_terminal_error(status);
    }
    cleanup_session();
    ESP_LOGI(TAG, "realtime session finished status=%s", esp_err_to_name(status));
    notify_done_once(status);
    vTaskDelete(NULL);
}

esp_err_t voice_duplex_volc_init(const voice_duplex_volc_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    EventGroupHandle_t events = xEventGroupCreate();
    if (!events) {
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_mux);
    if (s.initialized) {
        s.turn_done = config->turn_done;
        s.done = config->done;
        s.done_ctx = config->user_ctx;
        portEXIT_CRITICAL(&s_mux);
        vEventGroupDelete(events);
        return ESP_OK;
    }
    memset(&s, 0, sizeof(s));
    s.initialized = true;
    s.events = events;
    s.turn_done = config->turn_done;
    s.done = config->done;
    s.done_ctx = config->user_ctx;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "initialized endpoint=%s build=%s mode=half-duplex-multiturn",
             DOUBAO_ENDPOINT, DOUBAO_BUILD_TAG);
    return ESP_OK;
}

static esp_err_t load_api_key(void)
{
    s.api_key[0] = '\0';
    esp_err_t err = settings_store_get_string("db_key", s.api_key, sizeof(s.api_key), "");
    if (err != ESP_OK) return err;
    if (!s.api_key[0]) {
        err = settings_store_get_string("asr_key", s.api_key, sizeof(s.api_key), "");
        if (err != ESP_OK) return err;
    }
    if (!s.api_key[0]) {
        ESP_LOGW(TAG, "Doubao API key missing: configure db_key or existing asr_key");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t voice_duplex_volc_begin(uint32_t input_sample_rate_hz)
{
    if (!s.initialized || !s.events) return ESP_ERR_INVALID_STATE;
    if (input_sample_rate_hz != DOUBAO_INPUT_RATE_HZ) return ESP_ERR_NOT_SUPPORTED;

    portENTER_CRITICAL(&s_mux);
    if (s.active) {
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }
    s.active = true;
    s.accepting_input = true;
    s.callback_sent = false;
    s.commit_requested = false;
    s.commit_sent = false;
    s.abort_requested = false;
    s.close_requested = false;
    s.session_create_sent = false;
    s.response_started = false;
    s.response_done = false;
    s.asr_completed = false;
    s.asr_text_seen = false;
    s.audio_started = false;
    s.audio_done = false;
    s.playback_started = false;
    s.turn_callback_sent = false;
    s.waiting_followup = false;
    s.tool_pending = false;
    s.tool_in_progress = false;
    s.tool_stage_done_pending = false;
    s.awaiting_tool_response = false;
    s.turn_index = 1;
    s.conversation_tick = 0;
    s.audio_delta_events = 0;
    s.audio_delta_b64_bytes = 0;
    s.downlink_raw_abs_sum = 0;
    s.downlink_raw_signed_sum = 0;
    s.downlink_raw_samples = 0;
    s.downlink_raw_peak = 0;
    s.downlink_raw_zero_samples = 0;
    s.downlink_raw_clipped_samples = 0;
    s.downlink_raw_bytes = 0;
    s.downlink_diag_chunks = 0;
    s.downlink_resampled_abs_sum = 0;
    s.downlink_resampled_signed_sum = 0;
    s.downlink_resampled_samples = 0;
    s.downlink_resampled_peak = 0;
    s.downlink_resampled_zero_samples = 0;
    s.downlink_resampled_clipped_samples = 0;
    s.downlink_play_frames = 0;
    s.terminal_status = ESP_OK;
    s.commit_tick = 0;
    s.audio_done_tick = 0;
    s.response_progress_tick = 0;
    s.response_progress_events = 0;
    s.tx_head = s.tx_tail = s.tx_len = 0;
    s.capture_abs_sum = 0;
    s.capture_signed_sum = 0;
    s.capture_samples = 0;
    s.capture_peak = 0;
    s.capture_zero_samples = 0;
    s.capture_clipped_samples = 0;
    s.capture_push_calls = 0;
    s.rx_head = s.rx_tail = s.rx_len = 0;
    portEXIT_CRITICAL(&s_mux);

    xEventGroupClearBits(s.events, BIT_WS_CONNECTED | BIT_SESSION_READY |
                                   BIT_WS_ERROR | BIT_WORK_KICK |
                                   BIT_SESSION_CLOSED | BIT_UPLINK_STOPPED |
                                   BIT_UPLINK_KICK);

    esp_err_t err = load_api_key();
    if (err != ESP_OK) goto fail;

    ESP_LOGI(TAG, "begin memory internal_free=%u largest=%u psram_free=%u ws_buf=%u ws_stack=%u worker_stack=%u uplink_stack=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)DOUBAO_WS_BUFFER_BYTES, (unsigned)DOUBAO_WS_TASK_STACK,
             (unsigned)DOUBAO_WORKER_STACK, (unsigned)DOUBAO_UPLINK_STACK);

    s.tx_ring = heap_caps_malloc(DOUBAO_TX_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.rx_ring = heap_caps_malloc(DOUBAO_RX_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.decode_buf = heap_caps_malloc(DOUBAO_AUDIO_DECODE_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.tx_b64 = heap_caps_malloc(DOUBAO_TX_B64_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.tx_json = heap_caps_malloc(DOUBAO_TX_JSON_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.tool_event = heap_caps_malloc(DOUBAO_TOOL_EVENT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.tool_output = heap_caps_malloc(DOUBAO_TOOL_OUTPUT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.tool_reply = heap_caps_malloc(DOUBAO_TOOL_REPLY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.tx_ring || !s.rx_ring || !s.decode_buf || !s.tx_b64 || !s.tx_json ||
        !s.tool_event || !s.tool_output || !s.tool_reply) {
        ESP_LOGE(TAG,
                 "begin allocation failed stage=session_buffers tx=%p rx=%p dec=%p b64=%p json=%p tool_evt=%p tool_out=%p tool_reply=%p events=%p",
                 s.tx_ring, s.rx_ring, s.decode_buf, s.tx_b64, s.tx_json,
                 s.tool_event, s.tool_output, s.tool_reply, s.events);
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    uint32_t r0 = esp_random();
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    uint32_t r3 = esp_random();
    snprintf(s.session_id, sizeof(s.session_id),
             "%08" PRIx32 "-%04" PRIx32 "-%04" PRIx32 "-%04" PRIx32 "-%04" PRIx32 "%08" PRIx32,
             r0, (r1 >> 16) & 0xffffU, (r1 & 0x0fffU) | 0x4000U,
             (r2 & 0x3fffU) | 0x8000U, (r2 >> 16) & 0xffffU, r3);
    snprintf(s.auth_header, sizeof(s.auth_header), "X-Api-Key: %s\r\n", s.api_key);

    esp_websocket_client_config_t ws_cfg = {
        .uri = DOUBAO_ENDPOINT,
        .headers = s.auth_header,
        .buffer_size = DOUBAO_WS_BUFFER_BYTES,
        .task_stack = DOUBAO_WS_TASK_STACK,
        .network_timeout_ms = 10000,
        .disable_auto_reconnect = true,
        .ping_interval_sec = 10,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    s.ws = esp_websocket_client_init(&ws_cfg);
    if (!s.ws) {
        ESP_LOGE(TAG, "begin allocation failed stage=websocket_init internal_free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    err = esp_websocket_register_events(s.ws, WEBSOCKET_EVENT_ANY,
                                                 websocket_event_handler, NULL);
    if (err != ESP_OK) goto fail;

    TaskHandle_t uplink_task = xTaskCreateStaticPinnedToCore(
        duplex_uplink_worker, "doubao_uplink", DOUBAO_UPLINK_STACK, NULL,
        DOUBAO_UPLINK_PRIORITY, s_uplink_task_stack, &s_uplink_task_tcb,
        tskNO_AFFINITY);
    if (!uplink_task) {
        ESP_LOGE(TAG,
                 "begin failed stage=uplink_task internal_free=%u largest=%u stack=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)DOUBAO_UPLINK_STACK);
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    portENTER_CRITICAL(&s_mux);
    s.uplink_worker = uplink_task;
    portEXIT_CRITICAL(&s_mux);

    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(duplex_worker, "doubao_voice",
                                                 DOUBAO_WORKER_STACK, NULL,
                                                 DOUBAO_WORKER_PRIORITY, &task,
                                                 tskNO_AFFINITY);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "begin allocation failed stage=worker_task internal_free=%u largest=%u stack=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)DOUBAO_WORKER_STACK);
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    portENTER_CRITICAL(&s_mux);
    s.worker = task;
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "before websocket start internal_free=%u largest=%u ws_stack=%u worker_stack=%u uplink_stack=%u ws_buf=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)DOUBAO_WS_TASK_STACK, (unsigned)DOUBAO_WORKER_STACK,
             (unsigned)DOUBAO_UPLINK_STACK, (unsigned)DOUBAO_WS_BUFFER_BYTES);
    err = esp_websocket_client_start(s.ws);
    if (err != ESP_OK) {
        set_terminal_error(err);
        return ESP_OK; /* worker owns cleanup and completion callback */
    }
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "begin failed: %s", esp_err_to_name(err));
    portENTER_CRITICAL(&s_mux);
    s.close_requested = true;
    portEXIT_CRITICAL(&s_mux);
    if (s.events) {
        xEventGroupSetBits(s.events, BIT_WORK_KICK | BIT_UPLINK_KICK);
    }
    wait_for_uplink_stop();
    if (s.ws) {
        (void)esp_websocket_client_destroy(s.ws);
        s.ws = NULL;
    }
    free(s.tx_ring); s.tx_ring = NULL;
    free(s.rx_ring); s.rx_ring = NULL;
    free(s.decode_buf); s.decode_buf = NULL;
    free(s.tx_b64); s.tx_b64 = NULL;
    free(s.tx_json); s.tx_json = NULL;
    free(s.tool_event); s.tool_event = NULL;
    free(s.tool_output); s.tool_output = NULL;
    free(s.tool_reply); s.tool_reply = NULL;
    portENTER_CRITICAL(&s_mux);
    s.active = false;
    s.accepting_input = false;
    s.worker = NULL;
    s.uplink_worker = NULL;
    portEXIT_CRITICAL(&s_mux);
    notify_done_once(err);
    return err;
}

esp_err_t voice_duplex_volc_wait_ready(uint32_t timeout_ms)
{
    if (!voice_duplex_volc_is_busy() || !s.events) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s.events, BIT_SESSION_READY | BIT_WS_ERROR,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_SESSION_READY) {
        return ESP_OK;
    }
    if (bits & BIT_WS_ERROR) {
        esp_err_t err;
        portENTER_CRITICAL(&s_mux);
        err = s.terminal_status;
        portEXIT_CRITICAL(&s_mux);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    return ESP_ERR_TIMEOUT;
}


esp_err_t voice_duplex_volc_push_pcm(const int16_t *samples, size_t sample_count)
{
    if (!samples || sample_count == 0) return ESP_ERR_INVALID_ARG;

    uint64_t abs_sum = 0;
    int64_t signed_sum = 0;
    uint32_t peak = 0;
    uint32_t zeros = 0;
    uint32_t clipped = 0;
    for (size_t i = 0; i < sample_count; ++i) {
        int32_t v = samples[i];
        int32_t mag = v < 0 ? -v : v;
        abs_sum += (uint32_t)mag;
        signed_sum += v;
        if ((uint32_t)mag > peak) peak = (uint32_t)mag;
        if (v == 0) zeros++;
        if (v == INT16_MAX || v == INT16_MIN) clipped++;
    }

    size_t bytes = sample_count * sizeof(int16_t);
    size_t dropped = 0;
    size_t wrote;
    EventGroupHandle_t events;
    portENTER_CRITICAL(&s_mux);
    if (!s.active || !s.accepting_input || !s.tx_ring) {
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }
    s.capture_abs_sum += abs_sum;
    s.capture_signed_sum += signed_sum;
    s.capture_samples += (uint32_t)sample_count;
    if (peak > s.capture_peak) s.capture_peak = peak;
    s.capture_zero_samples += zeros;
    s.capture_clipped_samples += clipped;
    s.capture_push_calls++;

    /*
     * Uplink overflow must not tear the session down: the microphone keeps
     * producing while the socket is busy, so keep the newest audio and drop the
     * oldest whole transport packets. Every drop is logged and included in the
     * commit diagnostics, because losing speech here would explain ASR stalls.
     */
    if (bytes > DOUBAO_TX_RING_BYTES - s.tx_len) {
        size_t need = bytes - (DOUBAO_TX_RING_BYTES - s.tx_len);
        need = ((need + DOUBAO_PACKET_BYTES - 1U) / DOUBAO_PACKET_BYTES) *
               DOUBAO_PACKET_BYTES;
        dropped = ring_drop_locked(s.tx_ring, DOUBAO_TX_RING_BYTES,
                                   &s.tx_tail, &s.tx_len, need);
    }
    wrote = ring_write_locked(s.tx_ring, DOUBAO_TX_RING_BYTES,
                              &s.tx_head, &s.tx_len, (const uint8_t *)samples, bytes);
    events = s.events;
    portEXIT_CRITICAL(&s_mux);
    if (dropped > 0) {
        ESP_LOGW(TAG, "uplink ring overflow: dropped %u oldest bytes", (unsigned)dropped);
    }
    if (wrote != bytes) {
        ESP_LOGE(TAG, "uplink ring overflow need=%u queued=%u", (unsigned)bytes,
                 (unsigned)tx_len_snapshot());
        return ESP_ERR_NO_MEM;
    }
    if (events) xEventGroupSetBits(events, BIT_UPLINK_KICK);
    return ESP_OK;
}

esp_err_t voice_duplex_volc_commit(void)
{
    uint32_t samples = 0;
    uint32_t peak = 0;
    uint32_t zeros = 0;
    uint32_t clipped = 0;
    uint32_t pushes = 0;
    uint64_t abs_sum = 0;
    int64_t signed_sum = 0;
    size_t queued = 0;
    EventGroupHandle_t events;
    portENTER_CRITICAL(&s_mux);
    if (!s.active || !s.accepting_input) {
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }
    s.accepting_input = false;
    s.commit_requested = true;
    samples = s.capture_samples;
    peak = s.capture_peak;
    zeros = s.capture_zero_samples;
    clipped = s.capture_clipped_samples;
    pushes = s.capture_push_calls;
    abs_sum = s.capture_abs_sum;
    signed_sum = s.capture_signed_sum;
    queued = s.tx_len;
    events = s.events;
    portEXIT_CRITICAL(&s_mux);

    uint32_t mean_abs = samples ? (uint32_t)(abs_sum / samples) : 0U;
    int32_t dc_mean = samples ? (int32_t)(signed_sum / (int64_t)samples) : 0;
    uint32_t zero_permille = samples ? (uint32_t)(((uint64_t)zeros * 1000U) / samples) : 0U;
    uint32_t audio_ms = samples ? (uint32_t)(((uint64_t)samples * 1000U) / DOUBAO_INPUT_RATE_HZ) : 0U;
    ESP_LOGI(TAG,
             "uplink captured pushes=%u samples=%u bytes=%u audio_ms=%u mean_abs=%u peak=%u dc=%d zeros_permille=%u clipped=%u queued_now=%u",
             (unsigned)pushes, (unsigned)samples, (unsigned)(samples * 2U),
             (unsigned)audio_ms, (unsigned)mean_abs, (unsigned)peak,
             (int)dc_mean, (unsigned)zero_permille, (unsigned)clipped,
             (unsigned)queued);

    if (events) xEventGroupSetBits(events, BIT_UPLINK_KICK);
    ESP_LOGI(TAG, "microphone commit requested build=%s queued=%u", DOUBAO_BUILD_TAG,
             (unsigned)queued);
    return ESP_OK;
}

esp_err_t voice_duplex_volc_resume_turn(void)
{
    EventGroupHandle_t events = NULL;
    portENTER_CRITICAL(&s_mux);
    if (!s.active || !s.waiting_followup || s.abort_requested ||
        s.close_requested || !s.ws || !s.tx_ring || !s.rx_ring) {
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    s.turn_index++;
    s.accepting_input = true;
    s.commit_requested = false;
    s.commit_sent = false;
    s.response_started = false;
    s.response_done = false;
    s.asr_completed = false;
    s.asr_text_seen = false;
    s.audio_started = false;
    s.audio_done = false;
    s.playback_started = false;
    s.turn_callback_sent = false;
    s.waiting_followup = false;
    s.tool_pending = false;
    s.tool_in_progress = false;
    s.tool_stage_done_pending = false;
    s.awaiting_tool_response = false;
    s.audio_delta_events = 0;
    s.audio_delta_b64_bytes = 0;
    s.downlink_raw_abs_sum = 0;
    s.downlink_raw_signed_sum = 0;
    s.downlink_raw_samples = 0;
    s.downlink_raw_peak = 0;
    s.downlink_raw_zero_samples = 0;
    s.downlink_raw_clipped_samples = 0;
    s.downlink_raw_bytes = 0;
    s.downlink_diag_chunks = 0;
    s.downlink_resampled_abs_sum = 0;
    s.downlink_resampled_signed_sum = 0;
    s.downlink_resampled_samples = 0;
    s.downlink_resampled_peak = 0;
    s.downlink_resampled_zero_samples = 0;
    s.downlink_resampled_clipped_samples = 0;
    s.downlink_play_frames = 0;
    s.tx_head = s.tx_tail = s.tx_len = 0;
    s.rx_head = s.rx_tail = s.rx_len = 0;
    s.capture_abs_sum = 0;
    s.capture_signed_sum = 0;
    s.capture_samples = 0;
    s.capture_peak = 0;
    s.capture_zero_samples = 0;
    s.capture_clipped_samples = 0;
    s.capture_push_calls = 0;
    s.commit_tick = 0;
    s.audio_done_tick = 0;
    s.response_progress_tick = 0;
    s.response_progress_events = 0;
    if (s.tool_event) {
        s.tool_event[0] = '\0';
    }
    events = s.events;
    uint32_t turn_index = s.turn_index;
    portEXIT_CRITICAL(&s_mux);

    if (events) {
        xEventGroupSetBits(events, BIT_WORK_KICK | BIT_UPLINK_KICK);
    }
    ESP_LOGI(TAG, "follow-up turn=%u accepted without reconnect",
             (unsigned)turn_index);
    return ESP_OK;
}

esp_err_t voice_duplex_volc_close(void)
{
    EventGroupHandle_t events = NULL;
    portENTER_CRITICAL(&s_mux);
    if (!s.active) {
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }
    s.accepting_input = false;
    s.close_requested = true;
    events = s.events;
    portEXIT_CRITICAL(&s_mux);

    if (events) {
        xEventGroupSetBits(events, BIT_WORK_KICK | BIT_UPLINK_KICK);
    }
    return ESP_OK;
}

void voice_duplex_volc_abort(void)
{
    EventGroupHandle_t events;
    portENTER_CRITICAL(&s_mux);
    bool active = s.active;
    if (active) {
        s.accepting_input = false;
        s.abort_requested = true;
    }
    events = s.events;
    portEXIT_CRITICAL(&s_mux);
    if (active && events) {
        xEventGroupSetBits(events, BIT_WORK_KICK | BIT_UPLINK_KICK);
    }
}

bool voice_duplex_volc_is_busy(void)
{
    bool active;
    portENTER_CRITICAL(&s_mux);
    active = s.active;
    portEXIT_CRITICAL(&s_mux);
    return active;
}
