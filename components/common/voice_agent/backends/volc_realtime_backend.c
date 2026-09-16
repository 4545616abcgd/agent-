#include "volc_realtime_backend.h"

#include "esp_log.h"
#include "voice_duplex_volc.h"

static const char *TAG = "volc_backend";

static volc_backend_done_fn s_turn_done;
static volc_backend_done_fn s_session_done;
static void *s_done_ctx;
static bool s_initialized;

static void transport_turn_done(esp_err_t status, void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "transport turn done status=%s", esp_err_to_name(status));
    if (s_turn_done) {
        s_turn_done(status, s_done_ctx);
    }
}

static void transport_session_done(esp_err_t status, void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "transport session done status=%s", esp_err_to_name(status));
    if (s_session_done) {
        s_session_done(status, s_done_ctx);
    }
}

esp_err_t volc_backend_init(volc_backend_done_fn turn_done,
                            volc_backend_done_fn session_done,
                            void *ctx)
{
    s_turn_done = turn_done;
    s_session_done = session_done;
    s_done_ctx = ctx;

    voice_duplex_volc_config_t cfg = {
        .turn_done = transport_turn_done,
        .done = transport_session_done,
        .user_ctx = NULL,
    };
    esp_err_t err = voice_duplex_volc_init(&cfg);
    if (err == ESP_OK) {
        s_initialized = true;
        ESP_LOGI(TAG, "Doubao realtime backend ready");
    }
    return err;
}

esp_err_t volc_backend_start(uint32_t sample_rate_hz)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = voice_duplex_volc_begin(sample_rate_hz);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Do not tell Voice Service that streaming is ready merely because the
     * transport task was spawned.  The user-facing beep must happen only after
     * session.created, otherwise microphone PCM accumulates before the cloud
     * can accept it and strict realtime pacing becomes impossible.
     */
    err = voice_duplex_volc_wait_ready(10000U);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Doubao session ready wait failed: %s", esp_err_to_name(err));
        voice_duplex_volc_abort();
        return err;
    }

    ESP_LOGI(TAG, "Doubao session ready; microphone may start now");
    return ESP_OK;
}

esp_err_t volc_backend_send_audio(const int16_t *pcm, size_t samples)
{
    return voice_duplex_volc_push_pcm(pcm, samples);
}

esp_err_t volc_backend_commit(void)
{
    return voice_duplex_volc_commit();
}

esp_err_t volc_backend_resume_turn(void)
{
    return voice_duplex_volc_resume_turn();
}

esp_err_t volc_backend_close(void)
{
    return voice_duplex_volc_close();
}

esp_err_t volc_backend_interrupt(void)
{
    voice_duplex_volc_abort();
    return ESP_OK;
}

bool volc_backend_is_active(void)
{
    return voice_duplex_volc_is_busy();
}
