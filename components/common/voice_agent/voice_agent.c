#include "voice_agent.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "realtime_session.h"

static const char *TAG = "voice_agent";

static voice_agent_config_t s_cfg;
static voice_agent_state_t s_state = VOICE_AGENT_IDLE;
static bool s_initialized;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void set_state(voice_agent_state_t state)
{
    voice_agent_state_fn cb = NULL;
    void *ctx = NULL;

    portENTER_CRITICAL(&s_mux);
    s_state = state;
    cb = s_cfg.on_state_change;
    ctx = s_cfg.ctx;
    portEXIT_CRITICAL(&s_mux);

    if (cb) {
        cb(state, ctx);
    }
}

static void turn_done(esp_err_t status, void *ctx)
{
    (void)ctx;
    voice_agent_done_fn done = NULL;
    void *done_ctx = NULL;

    set_state(status == ESP_OK ? VOICE_AGENT_WAITING_FOLLOWUP : VOICE_AGENT_ERROR);

    portENTER_CRITICAL(&s_mux);
    done = s_cfg.on_turn_done;
    done_ctx = s_cfg.ctx;
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "turn complete status=%s", esp_err_to_name(status));
    if (done) {
        done(status, done_ctx);
    }
}

static void session_done(esp_err_t status, void *ctx)
{
    (void)ctx;
    voice_agent_done_fn done = NULL;
    void *done_ctx = NULL;

    set_state(status == ESP_OK ? VOICE_AGENT_IDLE : VOICE_AGENT_ERROR);

    portENTER_CRITICAL(&s_mux);
    done = s_cfg.on_session_done;
    done_ctx = s_cfg.ctx;
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "session complete status=%s", esp_err_to_name(status));
    if (done) {
        done(status, done_ctx);
    }

    /* An error only describes the completed turn; the agent is ready to re-arm. */
    if (status != ESP_OK) {
        set_state(VOICE_AGENT_IDLE);
    }
}

esp_err_t voice_agent_init(const voice_agent_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_mux);
    if (s_initialized) {
        s_cfg = *config;
        portEXIT_CRITICAL(&s_mux);
        return ESP_OK;
    }
    s_cfg = *config;
    portEXIT_CRITICAL(&s_mux);

    esp_err_t err = realtime_session_init(turn_done, session_done, NULL);
    if (err != ESP_OK) {
        set_state(VOICE_AGENT_ERROR);
        return err;
    }

    portENTER_CRITICAL(&s_mux);
    s_initialized = true;
    portEXIT_CRITICAL(&s_mux);
    set_state(VOICE_AGENT_IDLE);
    ESP_LOGI(TAG, "initialized realtime-only voice path");
    return ESP_OK;
}

esp_err_t voice_agent_start_session(uint32_t sample_rate_hz)
{
    if (!s_initialized || realtime_session_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }

    set_state(VOICE_AGENT_CONNECTING);
    esp_err_t err = realtime_session_start(sample_rate_hz);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "session start failed: %s", esp_err_to_name(err));
        set_state(VOICE_AGENT_IDLE);
        return err;
    }

    set_state(VOICE_AGENT_STREAMING);
    ESP_LOGI(TAG, "session start rate=%u", (unsigned)sample_rate_hz);
    return ESP_OK;
}

esp_err_t voice_agent_push_pcm(const int16_t *pcm, size_t samples)
{
    if (!pcm || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!realtime_session_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    return realtime_session_send_audio(pcm, samples);
}

esp_err_t voice_agent_stop_session(void)
{
    if (!realtime_session_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = realtime_session_commit();
    if (err == ESP_OK) {
        set_state(VOICE_AGENT_WAITING_RESPONSE);
        ESP_LOGI(TAG, "microphone capture closed; transport commit queued");
    }
    return err;
}

esp_err_t voice_agent_resume_turn(void)
{
    if (!s_initialized || !realtime_session_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = realtime_session_resume_turn();
    if (err == ESP_OK) {
        set_state(VOICE_AGENT_STREAMING);
        ESP_LOGI(TAG, "follow-up turn streaming on existing session");
    }
    return err;
}

esp_err_t voice_agent_close_session(void)
{
    if (!s_initialized || !realtime_session_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "closing multi-turn conversation");
    return realtime_session_close();
}

esp_err_t voice_agent_interrupt(void)
{
    if (!realtime_session_is_active()) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "interrupt realtime turn");
    return realtime_session_interrupt();
}

bool voice_agent_is_active(void)
{
    return realtime_session_is_active();
}

voice_agent_state_t voice_agent_get_state(void)
{
    voice_agent_state_t state;
    portENTER_CRITICAL(&s_mux);
    state = s_state;
    portEXIT_CRITICAL(&s_mux);
    return state;
}
