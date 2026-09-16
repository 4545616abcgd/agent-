#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_AGENT_IDLE = 0,
    VOICE_AGENT_CONNECTING,
    VOICE_AGENT_STREAMING,
    VOICE_AGENT_WAITING_RESPONSE,
    VOICE_AGENT_WAITING_FOLLOWUP,
    VOICE_AGENT_ERROR,
} voice_agent_state_t;

typedef void (*voice_agent_state_fn)(voice_agent_state_t state, void *ctx);
typedef void (*voice_agent_done_fn)(esp_err_t status, void *ctx);

typedef struct {
    voice_agent_state_fn on_state_change;
    voice_agent_done_fn on_turn_done;
    voice_agent_done_fn on_session_done;
    void *ctx;
} voice_agent_config_t;

/** Initialize the realtime voice agent once at boot. */
esp_err_t voice_agent_init(const voice_agent_config_t *config);

/** Start one cloud realtime turn using PCM16 mono at sample_rate_hz. */
esp_err_t voice_agent_start_session(uint32_t sample_rate_hz);

/** Push microphone PCM into the active realtime turn. Non-blocking. */
esp_err_t voice_agent_push_pcm(const int16_t *pcm, size_t samples);

/** Commit microphone input. The session stays active until cloud response ends. */
esp_err_t voice_agent_stop_session(void);

/** Resume microphone streaming for a follow-up turn without reconnecting. */
esp_err_t voice_agent_resume_turn(void);

/** Gracefully close the active multi-turn conversation. */
esp_err_t voice_agent_close_session(void);

/** Abort the current realtime turn. */
esp_err_t voice_agent_interrupt(void);

/** True while a realtime cloud turn is active. */
bool voice_agent_is_active(void);

voice_agent_state_t voice_agent_get_state(void);

#ifdef __cplusplus
}
#endif
