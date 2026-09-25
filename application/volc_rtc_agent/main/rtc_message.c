/* SPDX-License-Identifier: Apache-2.0 */

#include "rtc_message.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "rtc_message";

#define RTC_MESSAGE_MAX_BYTES 4096U
#define RTC_TOOL_OUTPUT_BYTES 2048U
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

__attribute__((weak)) esp_err_t rtc_message_dispatch_tool(
    const char *name,
    const char *arguments_json,
    char *output_json,
    size_t output_capacity)
{
    (void)name;
    (void)arguments_json;
    if (output_json && output_capacity > 0) {
        snprintf(output_json, output_capacity,
                 "{\"ok\":false,\"error\":\"ESP-Claw capability bridge is not linked\"}");
    }
    return ESP_ERR_NOT_SUPPORTED;
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
        if (!name || !name[0]) {
            ESP_LOGW(TAG, "ignoring malformed tool call");
            continue;
        }
        if (!arguments || !arguments[0]) {
            arguments = "{}";
        }
        s_tool_output[0] = '\0';
        esp_err_t err = rtc_message_dispatch_tool(
            name, arguments, s_tool_output, sizeof(s_tool_output));
        ESP_LOGI(TAG, "tool parsed id=%s name=%s dispatch=%s output_bytes=%u",
                 call_id ? call_id : "?", name, esp_err_to_name(err),
                 (unsigned)strlen(s_tool_output));
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
    if (payload_size == 0 || payload_size > packet->size - 8U) {
        ESP_LOGW(TAG,
                 "malformed RTC message prefix %.4s declared=%u received=%u",
                 (const char *)packet->data, (unsigned)payload_size,
                 (unsigned)packet->size);
        return;
    }

    cJSON *root = cJSON_ParseWithLength(
        (const char *)packet->data + 8, payload_size);
    if (!root) {
        ESP_LOGW(TAG, "RTC message JSON parse failed");
        return;
    }
    if (memcmp(packet->data, "subv", 4) == 0) {
        process_subtitle(root);
    } else if (memcmp(packet->data, "tool", 4) == 0) {
        process_tool_calls(root);
    } else if (memcmp(packet->data, "conv", 4) == 0) {
        process_conversation_state(root);
    } else {
        ESP_LOGI(TAG, "unhandled RTC message prefix %.4s size=%u",
                 (const char *)packet->data, (unsigned)packet->size);
    }
    cJSON_Delete(root);
}

static void message_task(void *arg)
{
    (void)arg;
    for (;;) {
        rtc_message_packet_t *packet = NULL;
        if (xQueueReceive(s_message_ctx.ready_queue, &packet,
                          portMAX_DELAY) != pdPASS || !packet) {
            continue;
        }
        process_packet(packet);
        if (xQueueSend(s_message_ctx.free_queue, &packet, 0) != pdPASS) {
            ESP_LOGE(TAG, "message pool corruption while returning packet");
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
        (void)xQueueSend(s_message_ctx.free_queue, &packet, 0);
    }
}
