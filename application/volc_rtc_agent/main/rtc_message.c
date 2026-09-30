/* SPDX-License-Identifier: Apache-2.0 */

#include "rtc_message.h"
#include "rtc_agent.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "rtc_message";

#define RTC_MESSAGE_MAX_BYTES 4096U
#define RTC_TOOL_OUTPUT_BYTES 8192U
#define RTC_TOOL_REPLY_BYTES 16384U
#define RTC_MESSAGE_QUEUE_LEN  4U
#define RTC_MESSAGE_TASK_STACK_BYTES 6144U
#define RTC_MESSAGE_TASK_STACK_WORDS \
    (RTC_MESSAGE_TASK_STACK_BYTES / sizeof(StackType_t))

typedef struct {
    uint16_t size;
    bool binary;
    uint8_t data[RTC_MESSAGE_MAX_BYTES];
} rtc_message_packet_t;

typedef struct {
    QueueHandle_t ready_queue;
    QueueHandle_t free_queue;
    StaticQueue_t ready_queue_ctrl;
    StaticQueue_t free_queue_ctrl;
    uint8_t ready_queue_storage[
        RTC_MESSAGE_QUEUE_LEN * sizeof(rtc_message_packet_t *)];
    uint8_t free_queue_storage[
        RTC_MESSAGE_QUEUE_LEN * sizeof(rtc_message_packet_t *)];
    StaticTask_t task_ctrl;
    TaskHandle_t task;
    uint32_t drops;
    bool started;
} rtc_message_ctx_t;

static rtc_message_ctx_t s_message_ctx;
static StackType_t s_message_task_stack[RTC_MESSAGE_TASK_STACK_WORDS];
EXT_RAM_BSS_ATTR static rtc_message_packet_t
    s_message_packets[RTC_MESSAGE_QUEUE_LEN];
EXT_RAM_BSS_ATTR static char s_tool_output[RTC_TOOL_OUTPUT_BYTES];
EXT_RAM_BSS_ATTR static uint8_t s_tool_reply[RTC_TOOL_REPLY_BYTES];

#if !CONFIG_APP_VOLC_RTC_VOICE
void rtc_message_probe_tools(char *output, size_t capacity)
{
    (void)output;
    (void)capacity;
}

esp_err_t rtc_message_dispatch_tool(
    const char *call_id,
    const char *name,
    const char *arguments_json,
    char *output_json,
    size_t output_capacity)
{
    (void)call_id;
    (void)name;
    (void)arguments_json;
    if (output_json && output_capacity > 0) {
        snprintf(output_json, output_capacity,
                 "{\"ok\":false,\"error\":\"ESP-Claw capability bridge is not linked\"}");
    }
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

static bool append_text(size_t *used, const char *text)
{
    size_t length = strlen(text);
    if (length > sizeof(s_tool_reply) - *used) {
        return false;
    }
    memcpy(s_tool_reply + *used, text, length);
    *used += length;
    return true;
}

static bool append_json_string(size_t *used, const char *text)
{
    static const char hex[] = "0123456789abcdef";
    if (!append_text(used, "\"")) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        char escape[7] = {0};
        if (*p == '"' || *p == '\\') {
            escape[0] = '\\';
            escape[1] = (char)*p;
        } else if (*p < 0x20U) {
            memcpy(escape, "\\u00", 4);
            escape[4] = hex[*p >> 4];
            escape[5] = hex[*p & 0x0f];
        } else {
            escape[0] = (char)*p;
        }
        if (!append_text(used, escape)) {
            return false;
        }
    }
    return append_text(used, "\"");
}

static esp_err_t send_tool_result(const char *call_id, const char *content)
{
    size_t used = 8U;
    if (!append_text(&used, "{\"ToolCallID\":") ||
        !append_json_string(&used, call_id) ||
        !append_text(&used, ",\"Content\":") ||
        !append_json_string(&used, content) ||
        !append_text(&used, "}")) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t payload_size = used - 8U;
    memcpy(s_tool_reply, "func", 4);
    s_tool_reply[4] = (uint8_t)(payload_size >> 24);
    s_tool_reply[5] = (uint8_t)(payload_size >> 16);
    s_tool_reply[6] = (uint8_t)(payload_size >> 8);
    s_tool_reply[7] = (uint8_t)payload_size;
    return rtc_agent_send_tool_message(s_tool_reply, used);
}

static void process_subtitle(const cJSON *root)
{
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(items)) {
        return;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        const char *user = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(item, "userId"));
        const char *text = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(item, "text"));
        if (text && text[0]) {
            ESP_LOGI(TAG, "subtitle user=%s text=%s", user ? user : "?", text);
        }
    }
}

static void process_tool_calls(const cJSON *root)
{
    const cJSON *calls = cJSON_GetObjectItemCaseSensitive(root, "tool_calls");
    if (!cJSON_IsArray(calls)) {
        ESP_LOGW(TAG, "tool message has no tool_calls array");
        return;
    }

    const cJSON *call = NULL;
    cJSON_ArrayForEach(call, calls) {
        const char *call_id = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(call, "id"));
        const cJSON *function =
            cJSON_GetObjectItemCaseSensitive(call, "function");
        const char *name = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(function, "name"));
        const char *arguments = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(function, "arguments"));
        if (!call_id || !call_id[0] || !name || !name[0]) {
            ESP_LOGW(TAG, "ignoring malformed tool call");
            continue;
        }
        const cJSON *argument_item =
            cJSON_GetObjectItemCaseSensitive(function, "arguments");
        if (argument_item && !cJSON_IsString(argument_item)) {
            ESP_LOGW(TAG, "tool id=%s has non-string arguments", call_id);
            esp_err_t send_err = send_tool_result(
                call_id, "{\"ok\":false,\"error\":\"arguments must be a JSON object encoded as a string\"}");
            ESP_LOGI(TAG, "tool id=%s rejected send=%s",
                     call_id, esp_err_to_name(send_err));
            continue;
        }
        if (!arguments || !arguments[0]) {
            arguments = "{}";
        }
        ESP_LOGI(TAG, "tool received id=%s name=%s argument_bytes=%u",
                 call_id, name, (unsigned)strlen(arguments));
        int64_t started_us = esp_timer_get_time();
        s_tool_output[0] = '\0';
        esp_err_t err = rtc_message_dispatch_tool(
            call_id, name, arguments, s_tool_output, sizeof(s_tool_output));
        if (!s_tool_output[0]) {
            snprintf(s_tool_output, sizeof(s_tool_output),
                     "{\"ok\":false,\"error\":\"tool call failed\",\"code\":\"%s\"}",
                     esp_err_to_name(err));
        }
        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);
        esp_err_t send_err = send_tool_result(call_id, s_tool_output);
        if (send_err == ESP_ERR_INVALID_SIZE) {
            ESP_LOGW(TAG, "tool id=%s result too large after JSON escaping; returning error", call_id);
            send_err = send_tool_result(call_id,
                "{\"ok\":false,\"error\":\"tool result exceeds RTC reply capacity\"}");
        }
        ESP_LOGI(TAG, "tool id=%s name=%s dispatch=%s send=%s output_bytes=%u elapsed_ms=%u stack_free_min=%u",
                 call_id, name, esp_err_to_name(err), esp_err_to_name(send_err),
                 (unsigned)strlen(s_tool_output), (unsigned)elapsed_ms,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

static void process_info(const cJSON *root)
{
    const char *event = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(root, "event_type"));
    if (event && strcmp(event, "function_calling") == 0) {
        const char *name = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(root, "function"));
        const char *id = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(root, "tool_call_id"));
        ESP_LOGI(TAG, "tool announced id=%s name=%s (awaiting tool arguments)",
                 id ? id : "?", name ? name : "?");
    }
}

static void process_conversation_state(const cJSON *root)
{
    const cJSON *stage = cJSON_GetObjectItemCaseSensitive(root, "Stage");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(stage, "Code");
    const char *description = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(stage, "Description"));

    if (!cJSON_IsObject(stage) || !cJSON_IsNumber(code)) {
        ESP_LOGW(TAG, "malformed conversation state message");
        return;
    }
    ESP_LOGI(TAG, "conversation state code=%d description=%s",
             code->valueint, description ? description : "");
}

static void process_packet(rtc_message_packet_t *packet)
{
    if (!packet || packet->size <= 8U ||
        packet->size >= RTC_MESSAGE_MAX_BYTES) {
        return;
    }

    const uint8_t *bytes = packet->data;
    size_t payload_size = ((size_t)bytes[4] << 24) |
                          ((size_t)bytes[5] << 16) |
                          ((size_t)bytes[6] << 8) |
                          (size_t)bytes[7];
    if (!packet->binary || payload_size == 0 || payload_size != packet->size - 8U) {
        ESP_LOGW(TAG,
                 "malformed RTC message prefix %.4s declared=%u received=%u",
                 (const char *)packet->data, (unsigned)payload_size,
                 (unsigned)packet->size);
        return;
    }

    const char *payload = (const char *)packet->data + 8;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(payload, payload_size, &end, false);
    while (end && end < payload + payload_size &&
           (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        ++end;
    }
    if (!cJSON_IsObject(root) || end != payload + payload_size) {
        ESP_LOGW(TAG, "RTC message JSON parse failed");
        cJSON_Delete(root);
        return;
    }
    if (memcmp(packet->data, "subv", 4) == 0) {
        process_subtitle(root);
    } else if (memcmp(packet->data, "tool", 4) == 0) {
        process_tool_calls(root);
    } else if (memcmp(packet->data, "conv", 4) == 0) {
        process_conversation_state(root);
    } else if (memcmp(packet->data, "info", 4) == 0) {
        process_info(root);
    } else {
        ESP_LOGI(TAG, "unhandled RTC message prefix %.4s size=%u",
                 (const char *)packet->data, (unsigned)packet->size);
    }
    cJSON_Delete(root);
}

static void message_task(void *arg)
{
    (void)arg;
    rtc_message_probe_tools(s_tool_output, sizeof(s_tool_output));
    ESP_LOGI(TAG, "message worker stack_free_min=%u after local probes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    int64_t followup_probe_us = esp_timer_get_time() + 10000000;
    for (;;) {
        rtc_message_packet_t *packet = NULL;
        if (xQueueReceive(s_message_ctx.ready_queue, &packet,
                         followup_probe_us ? pdMS_TO_TICKS(10000) : portMAX_DELAY) == pdPASS && packet) {
            process_packet(packet);
            if (xQueueSend(s_message_ctx.free_queue, &packet, 0) != pdPASS) {
                ESP_LOGE(TAG, "message pool corruption while returning packet");
            }
        }
        if (followup_probe_us && esp_timer_get_time() >= followup_probe_us) {
            /* The first post-SNTP weather fetch may still be in progress at boot. */
            rtc_message_probe_tools(s_tool_output, sizeof(s_tool_output));
            followup_probe_us = 0;
        }
    }
}

esp_err_t rtc_message_start(void)
{
    if (s_message_ctx.started) {
        return ESP_OK;
    }

    memset(&s_message_ctx, 0, sizeof(s_message_ctx));
    s_message_ctx.ready_queue = xQueueCreateStatic(
        RTC_MESSAGE_QUEUE_LEN, sizeof(rtc_message_packet_t *),
        s_message_ctx.ready_queue_storage,
        &s_message_ctx.ready_queue_ctrl);
    s_message_ctx.free_queue = xQueueCreateStatic(
        RTC_MESSAGE_QUEUE_LEN, sizeof(rtc_message_packet_t *),
        s_message_ctx.free_queue_storage,
        &s_message_ctx.free_queue_ctrl);
    ESP_RETURN_ON_FALSE(s_message_ctx.ready_queue && s_message_ctx.free_queue,
                        ESP_ERR_NO_MEM, TAG,
                        "static message queues failed to initialize");

    for (size_t i = 0; i < RTC_MESSAGE_QUEUE_LEN; ++i) {
        rtc_message_packet_t *packet = &s_message_packets[i];
        ESP_RETURN_ON_FALSE(
            xQueueSend(s_message_ctx.free_queue, &packet, 0) == pdPASS,
            ESP_FAIL, TAG, "message pool failed to initialize");
    }

    s_message_ctx.task = xTaskCreateStaticPinnedToCore(
        message_task, "rtc_message", RTC_MESSAGE_TASK_STACK_WORDS,
        NULL, 6, s_message_task_stack, &s_message_ctx.task_ctrl, 0);
    ESP_RETURN_ON_FALSE(s_message_ctx.task, ESP_ERR_NO_MEM, TAG,
                        "message task failed to start");
    s_message_ctx.started = true;
    ESP_LOGI(TAG, "RTC signaling worker ready pool=%u max_message=%u",
             (unsigned)RTC_MESSAGE_QUEUE_LEN,
             (unsigned)(RTC_MESSAGE_MAX_BYTES - 1U));
    return ESP_OK;
}

void rtc_message_process(const void *message,
                         size_t size,
                         bool binary)
{
    if (!s_message_ctx.started || !message || size <= 8U ||
        size >= RTC_MESSAGE_MAX_BYTES) {
        ESP_LOGW(TAG, "invalid RTC message size=%u", (unsigned)size);
        return;
    }

    rtc_message_packet_t *packet = NULL;
    if (xQueueReceive(s_message_ctx.free_queue, &packet, 0) != pdPASS ||
        !packet) {
        s_message_ctx.drops++;
        if ((s_message_ctx.drops % 20U) == 1U) {
            ESP_LOGW(TAG, "RTC signaling backpressure drops=%u",
                     (unsigned)s_message_ctx.drops);
        }
        return;
    }

    packet->size = (uint16_t)size;
    packet->binary = binary;
    memcpy(packet->data, message, size);
    if (xQueueSend(s_message_ctx.ready_queue, &packet, 0) != pdPASS) {
        s_message_ctx.drops++;
        if (xQueueSend(s_message_ctx.free_queue, &packet, 0) != pdPASS) {
            ESP_LOGE(TAG, "message pool corruption after enqueue failure");
        }
    }
}
