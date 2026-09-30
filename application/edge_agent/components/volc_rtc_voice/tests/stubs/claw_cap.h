#pragma once
#include <stddef.h>
#include "esp_err.h"
#define CLAW_CAP_CALLER_AGENT 1
typedef struct {
    const char *session_id;
    const char *channel;
    const char *chat_id;
    const char *source_cap;
    const char *correlation_id;
    int caller;
} claw_cap_call_context_t;
esp_err_t claw_cap_call(const char *name, const char *arguments,
    const claw_cap_call_context_t *context, char *output, size_t capacity);
