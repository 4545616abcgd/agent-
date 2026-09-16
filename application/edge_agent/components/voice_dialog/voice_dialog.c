/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * V2.4 compatibility adapter:
 *   WakeNet/Voice Service -> Voice Agent -> Doubao Realtime
 *
 * The historical ASR configuration API is kept only so the existing web UI
 * continues to compile.  It is no longer part of the wake-word voice path.
 */

#include "voice_dialog.h"

#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "settings_store.h"
#include "voice_agent.h"
#include "voice_reply_tts.h"
#include "voice_service.h"

static const char *TAG = "voice_dialog";

#define ASR_DEFAULT_URL   "https://api.siliconflow.cn/v1/audio/transcriptions"
#define ASR_DEFAULT_MODEL "FunAudioLLM/SenseVoiceSmall"
#define ASR_KEY_NVS       "asr_key"
#define ASR_URL_NVS       "asr_url"
#define ASR_MODEL_NVS     "asr_model"
#define VOICE_REARM_DELAY_US (150ULL * 1000ULL)

typedef enum {
    VOICE_TRANSITION_NONE = 0,
    VOICE_TRANSITION_FOLLOWUP,
    VOICE_TRANSITION_WAKE_LISTEN,
} voice_transition_t;

static esp_timer_handle_t s_transition_timer;
static portMUX_TYPE s_transition_mux = portMUX_INITIALIZER_UNLOCKED;
static voice_transition_t s_pending_transition;

static void close_session_after_followup_error(esp_err_t cause)
{
    ESP_LOGW(TAG, "follow-up capture could not start: %s; closing conversation",
             esp_err_to_name(cause));
    esp_err_t close_err = voice_agent_close_session();
    if (close_err != ESP_OK && close_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "conversation close failed: %s", esp_err_to_name(close_err));
    }
}

static void voice_rearm_timer_cb(void *arg)
{
    (void)arg;
    voice_transition_t transition;

    portENTER_CRITICAL(&s_transition_mux);
    transition = s_pending_transition;
    s_pending_transition = VOICE_TRANSITION_NONE;
    portEXIT_CRITICAL(&s_transition_mux);

    if (transition == VOICE_TRANSITION_FOLLOWUP) {
        if (voice_agent_get_state() != VOICE_AGENT_WAITING_FOLLOWUP) {
            ESP_LOGD(TAG, "stale follow-up transition ignored state=%d",
                     (int)voice_agent_get_state());
            return;
        }

        esp_err_t err = voice_agent_resume_turn();
        if (err != ESP_OK) {
            close_session_after_followup_error(err);
            return;
        }

        err = voice_service_start_followup();
        if (err != ESP_OK) {
            close_session_after_followup_error(err);
            return;
        }
        ESP_LOGI(TAG, "follow-up window opened on existing realtime session");
        return;
    }

    if (transition == VOICE_TRANSITION_WAKE_LISTEN) {
        esp_err_t err = voice_service_start();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Voice Service re-armed for wake word");
        } else if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGD(TAG, "Voice Service already running while re-arming");
        } else {
            ESP_LOGW(TAG, "Voice Service re-arm failed: %s", esp_err_to_name(err));
        }
    }
}

static void schedule_voice_transition(voice_transition_t transition)
{
    if (!s_transition_timer) {
        portENTER_CRITICAL(&s_transition_mux);
        s_pending_transition = transition;
        portEXIT_CRITICAL(&s_transition_mux);
        voice_rearm_timer_cb(NULL);
        return;
    }

    esp_err_t stop_err = esp_timer_stop(s_transition_timer);
    if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "voice transition timer stop failed: %s",
                 esp_err_to_name(stop_err));
    }
    portENTER_CRITICAL(&s_transition_mux);
    s_pending_transition = transition;
    portEXIT_CRITICAL(&s_transition_mux);

    esp_err_t err = esp_timer_start_once(s_transition_timer, VOICE_REARM_DELAY_US);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "voice transition timer start failed: %s; running directly",
                 esp_err_to_name(err));
        voice_rearm_timer_cb(NULL);
    }
}

static void agent_state_changed(voice_agent_state_t state, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "voice agent state=%d", (int)state);
}

static void agent_session_done(esp_err_t status, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "realtime conversation complete status=%s", esp_err_to_name(status));
    schedule_voice_transition(VOICE_TRANSITION_WAKE_LISTEN);
}

static void agent_turn_done(esp_err_t status, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "realtime turn complete status=%s", esp_err_to_name(status));
    if (status != ESP_OK) {
        close_session_after_followup_error(status);
        return;
    }
    schedule_voice_transition(VOICE_TRANSITION_FOLLOWUP);
}

static esp_err_t realtime_begin(uint32_t sample_rate_hz, void *user_ctx)
{
    (void)user_ctx;
    return voice_agent_start_session(sample_rate_hz);
}

static esp_err_t realtime_pcm(const int16_t *samples,
                              size_t sample_count,
                              uint32_t sample_rate_hz,
                              void *user_ctx)
{
    (void)sample_rate_hz;
    (void)user_ctx;
    return voice_agent_push_pcm(samples, sample_count);
}

static esp_err_t realtime_end(void *user_ctx)
{
    (void)user_ctx;
    return voice_agent_stop_session();
}

static esp_err_t realtime_close(void *user_ctx)
{
    (void)user_ctx;
    return voice_agent_close_session();
}

static void realtime_abort(void *user_ctx)
{
    (void)user_ctx;
    esp_err_t err = voice_agent_interrupt();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "realtime interrupt failed: %s", esp_err_to_name(err));
    }
}

bool voice_dialog_is_busy(void)
{
    return voice_agent_is_active();
}

esp_err_t voice_dialog_get_asr_config(voice_dialog_asr_config_t *out_config)
{
    if (!out_config) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_config, 0, sizeof(*out_config));

    esp_err_t err = settings_store_get_string(ASR_KEY_NVS,
                                               out_config->api_key,
                                               sizeof(out_config->api_key),
                                               "");
    if (err != ESP_OK) {
        return err;
    }
    err = settings_store_get_string(ASR_URL_NVS,
                                    out_config->base_url,
                                    sizeof(out_config->base_url),
                                    ASR_DEFAULT_URL);
    if (err != ESP_OK) {
        return err;
    }
    return settings_store_get_string(ASR_MODEL_NVS,
                                     out_config->model,
                                     sizeof(out_config->model),
                                     ASR_DEFAULT_MODEL);
}

esp_err_t voice_dialog_set_asr_config(const voice_dialog_asr_config_t *config)
{
    if (!config || !config->base_url[0] || !config->model[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(config->base_url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = settings_store_set_string(ASR_KEY_NVS, config->api_key);
    if (err != ESP_OK) {
        return err;
    }
    err = settings_store_set_string(ASR_URL_NVS, config->base_url);
    if (err != ESP_OK) {
        return err;
    }
    return settings_store_set_string(ASR_MODEL_NVS, config->model);
}

esp_err_t voice_dialog_init(void)
{
    if (!s_transition_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = voice_rearm_timer_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "voice_next",
        };
        esp_err_t timer_err = esp_timer_create(&timer_args, &s_transition_timer);
        if (timer_err != ESP_OK) {
            ESP_LOGW(TAG, "transition timer create failed: %s; direct fallback will be used",
                     esp_err_to_name(timer_err));
            s_transition_timer = NULL;
        }
    }

    const voice_agent_config_t agent_cfg = {
        .on_state_change = agent_state_changed,
        .on_turn_done = agent_turn_done,
        .on_session_done = agent_session_done,
        .ctx = NULL,
    };
    esp_err_t err = voice_agent_init(&agent_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "voice_agent init failed: %s", esp_err_to_name(err));
        return err;
    }

    const voice_service_realtime_sink_t realtime_sink = {
        .begin = realtime_begin,
        .pcm = realtime_pcm,
        .end = realtime_end,
        .close = realtime_close,
        .abort = realtime_abort,
    };
    err = voice_service_set_realtime_sink(&realtime_sink, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "realtime sink registration failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Explicitly disable the historical ASR -> text agent -> local TTS handoff. */
    err = voice_service_set_utterance_sink(NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "legacy utterance sink disable failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * The Web IM reply path still uses local ESP-TTS even though the wake-word
     * path now uses Doubao end to end. Load the SD voice set once while the
     * audio service is idle so the first Web reply does not race ESP-Claw's
     * storage work or pay a multi-second SD read on demand.
     */
    esp_err_t preload_err = voice_reply_tts_preload();
    if (preload_err == ESP_OK) {
        ESP_LOGI(TAG, "local ESP-TTS voice cache preloaded for Web replies");
    } else {
        ESP_LOGW(TAG,
                 "local ESP-TTS preload failed: %s; Web replies will retry on demand",
                 esp_err_to_name(preload_err));
    }

    ESP_LOGI(TAG, "V2.7 multi-turn realtime adapter ready; legacy ASR/TTS wake path disabled");
    return ESP_OK;
}
