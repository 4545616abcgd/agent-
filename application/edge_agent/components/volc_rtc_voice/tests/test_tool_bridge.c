/* Host protocol tests use real cJSON/parser/bridge, with only SDK calls stubbed. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define TAG rtc_message_TAG
#include "../../../../volc_rtc_agent/main/rtc_message.c"
#undef TAG
#include "../rtc_tool_bridge.c"

static unsigned dispatched;
static unsigned submitted;
static unsigned assertions;
static uint8_t sent[RTC_TOOL_REPLY_BYTES];
static size_t sent_size;
static const char *cap_result = "{\"ok\":true,\"temperature_c\":26}";

esp_err_t claw_cap_call(const char *name, const char *arguments,
    const claw_cap_call_context_t *context, char *output, size_t capacity)
{
    assert(is_allowed_tool(name));
    assert(arguments);
    assert(context->caller == CLAW_CAP_CALLER_AGENT);
    assert(strcmp(context->session_id, "volc-rtc") == 0);
    assert(context->correlation_id);
    ++dispatched;
    assert(strlen(cap_result) < capacity);
    strcpy(output, cap_result);
    return ESP_OK;
}

esp_err_t rtc_agent_send_tool_message(const void *data, size_t size)
{
    assert(size <= sizeof(sent));
    memcpy(sent, data, size);
    sent_size = size;
    ++submitted;
    return ESP_OK;
}

static void packet(const char *prefix, const char *json)
{
    rtc_message_packet_t input = {.binary = true};
    size_t size = strlen(json);
    assert(size + 8 < sizeof(input.data));
    input.size = (uint16_t)(size + 8);
    memcpy(input.data, prefix, 4);
    input.data[4] = (uint8_t)(size >> 24);
    input.data[5] = (uint8_t)(size >> 16);
    input.data[6] = (uint8_t)(size >> 8);
    input.data[7] = (uint8_t)size;
    memcpy(input.data + 8, json, size);
    process_packet(&input);
}

static void expect_reply(const char *id, const char *content)
{
    assert(sent_size > 8 && memcmp(sent, "func", 4) == 0);
    size_t length = ((size_t)sent[4] << 24) | ((size_t)sent[5] << 16) |
                    ((size_t)sent[6] << 8) | sent[7];
    assert(length == sent_size - 8);
    cJSON *reply = cJSON_ParseWithLength((const char *)sent + 8, length);
    assert(reply);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(reply, "ToolCallID")), id) == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(reply, "Content")), content) == 0);
    cJSON_Delete(reply);
    ++assertions;
}

static void check_arguments(const char *name, const char *json, bool expected)
{
    unsigned before = dispatched;
    char output[512];
    esp_err_t err = rtc_message_dispatch_tool("validation", name, json, output, sizeof(output));
    assert((err == ESP_OK) == expected);
    assert(dispatched == before + (expected ? 1U : 0U));
    ++assertions;
}

int main(void)
{
    packet("tool", "{\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"weather_get_current\",\"arguments\":\"{}\"}}]}");
    assert(dispatched == 1 && submitted == 1);
    expect_reply("call_1", cap_result);
    cap_result = "quotes \" slash \\ newline\n tab\t control\001 UTF8 \xe6\xb0\x94\xe8\xb1\xa1";
    assert(send_tool_result("call_\"\\", cap_result) == ESP_OK);
    expect_reply("call_\"\\", cap_result);
    check_arguments("weather_get_current", "{}", true);
    check_arguments("weather_get_current", "[]", false);
    check_arguments("weather_get_current", "{} trailing", false);
    check_arguments("weather_get_current", "{\"location\":\"other city\"}", false);
    check_arguments("weather_get_hourly", "{\"hours\":24}", true);
    check_arguments("weather_get_hourly", "{\"hours\":1.5}", false);
    check_arguments("weather_get_hourly", "{\"hours\":0}", false);
    check_arguments("weather_get_hourly", "{\"hours\":25}", false);
    check_arguments("weather_get_hourly", "{\"hours\":1,\"hours\":2}", false);
    check_arguments("weather_get_daily", "{\"days\":7}", true);
    check_arguments("weather_get_daily", "{\"days\":8}", false);
    check_arguments("weather_get_alerts", "{\"include_details\":true}", true);
    check_arguments("weather_get_alerts", "{\"include_details\":1}", false);
    check_arguments("get_system_info", "{\"sections\":[\"wifi\",\"memory\"]}", true);
    check_arguments("get_system_info", "{\"sections\":[]}", false);
    check_arguments("get_system_info", "{\"sections\":[\"secret\"]}", false);
    check_arguments("get_system_info", "{\"sections\":[\"wifi\",\"wifi\"]}", false);
    check_arguments("get_system_info", "{\"sections\":[1]}", false);
    check_arguments("get_current_time", "{}", true);
    check_arguments("restart_device", "{}", false);
    unsigned before = dispatched;
    packet("tool", "{\"tool_calls\":[{\"id\":\"bad_args\",\"function\":{\"name\":\"get_system_info\",\"arguments\":{}}}]}");
    assert(dispatched == before);
    expect_reply("bad_args", "{\"ok\":false,\"error\":\"arguments must be a JSON object encoded as a string\"}");
    unsigned before_send = submitted;
    packet("tool", "{\"tool_calls\":[]} {}");
    packet("tool", "not json");
    packet("tool", "{\"tool_calls\":[{\"function\":{\"name\":\"get_system_info\"}}]}");
    packet("info", "{\"event_type\":\"function_calling\",\"function\":\"get_system_info\",\"tool_call_id\":\"announced\"}");
    assert(dispatched == before && submitted == before_send);
    rtc_message_packet_t truncated = {.size = 10, .binary = true,
        .data = {'t','o','o','l',0,0,0,3,'{','}'}};
    process_packet(&truncated);
    truncated.data[7] = 1;
    process_packet(&truncated);
    truncated.data[7] = 2;
    truncated.binary = false;
    process_packet(&truncated);
    assert(dispatched == before && submitted == before_send);
    ++assertions;
    static char large[RTC_TOOL_OUTPUT_BYTES];
    memset(large, 1, sizeof(large) - 1);
    cap_result = large;
    packet("tool", "{\"tool_calls\":[{\"id\":\"large\",\"function\":{\"name\":\"get_system_info\",\"arguments\":\"{}\"}}]}");
    expect_reply("large", "{\"ok\":false,\"error\":\"tool result exceeds RTC reply capacity\"}");
    puts("PASS: real parser, validation, reply escaping, bounds and allowlist");
    printf("%u checks passed; cloud and FreeRTOS behavior are not simulated\n", assertions);
    return 0;
}
