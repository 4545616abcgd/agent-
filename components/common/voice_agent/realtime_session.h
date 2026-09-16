#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*realtime_session_done_fn)(esp_err_t status, void *ctx);

esp_err_t realtime_session_init(realtime_session_done_fn turn_done,
                                realtime_session_done_fn session_done,
                                void *ctx);
esp_err_t realtime_session_start(uint32_t sample_rate_hz);
esp_err_t realtime_session_send_audio(const int16_t *pcm, size_t samples);
esp_err_t realtime_session_commit(void);
esp_err_t realtime_session_resume_turn(void);
esp_err_t realtime_session_close(void);
esp_err_t realtime_session_interrupt(void);
bool realtime_session_is_active(void);
