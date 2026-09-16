#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*volc_backend_done_fn)(esp_err_t status, void *ctx);

esp_err_t volc_backend_init(volc_backend_done_fn turn_done,
                            volc_backend_done_fn session_done,
                            void *ctx);
esp_err_t volc_backend_start(uint32_t sample_rate_hz);
esp_err_t volc_backend_send_audio(const int16_t *pcm, size_t samples);
esp_err_t volc_backend_commit(void);
esp_err_t volc_backend_resume_turn(void);
esp_err_t volc_backend_close(void);
esp_err_t volc_backend_interrupt(void);
bool volc_backend_is_active(void);
