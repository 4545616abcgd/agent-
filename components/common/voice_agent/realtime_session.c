#include "realtime_session.h"

#include "backends/volc_realtime_backend.h"
#include "esp_log.h"

static const char *TAG = "realtime_session";

esp_err_t realtime_session_init(realtime_session_done_fn turn_done,
                                realtime_session_done_fn session_done,
                                void *ctx)
{
    esp_err_t err = volc_backend_init(turn_done, session_done, ctx);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "initialized backend=volc_realtime");
    }
    return err;
}

esp_err_t realtime_session_start(uint32_t sample_rate_hz)
{
    return volc_backend_start(sample_rate_hz);
}

esp_err_t realtime_session_send_audio(const int16_t *pcm, size_t samples)
{
    return volc_backend_send_audio(pcm, samples);
}

esp_err_t realtime_session_commit(void)
{
    return volc_backend_commit();
}

esp_err_t realtime_session_resume_turn(void)
{
    return volc_backend_resume_turn();
}

esp_err_t realtime_session_close(void)
{
    return volc_backend_close();
}

esp_err_t realtime_session_interrupt(void)
{
    return volc_backend_interrupt();
}

bool realtime_session_is_active(void)
{
    return volc_backend_is_active();
}
