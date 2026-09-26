/* SPDX-License-Identifier: Apache-2.0 */

#include "rtc_agent.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_afe_config.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_g711_dec.h"
#include "esp_g711_enc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "recorder_sr.h"
#include "rtc_audio.h"
#include "rtc_message.h"
#include "sdkconfig.h"
#include "volc_conv_ai.h"

static const char *TAG = "rtc_agent";

#define AGENT_BIT_WAKE          BIT0
#define AGENT_BIT_CONNECTED     BIT1
#define AGENT_BIT_DISCONNECTED  BIT2
#define AGENT_BIT_QUOTA         BIT3
#define AGENT_BIT_REMOTE_JOINED BIT4
#define AGENT_BIT_REMOTE_LEFT   BIT5
#define AGENT_BIT_TOKEN_EXPIRED BIT6
#define AGENT_BIT_LICENSE_WARNING BIT7
#define AGENT_BIT_CLOUD_READY   BIT8

#define AFE_FRAME_MS            20U
#define AFE_FRAME_SAMPLES       (RTC_AUDIO_SAMPLE_RATE_HZ * AFE_FRAME_MS / 1000U)
#define AFE_FRAME_BYTES         (AFE_FRAME_SAMPLES * sizeof(int16_t))
#define G711_SAMPLE_RATE_HZ     8000U
#define G711_PACKET_MS          60U
#define G711_PACKET_SAMPLES     (G711_SAMPLE_RATE_HZ * G711_PACKET_MS / 1000U)
#define G711_PACKET_BYTES       G711_PACKET_SAMPLES
#define DOWNLINK_PACKET_BYTES   1024U
#define DOWNLINK_QUEUE_LEN      8U
#define RTC_ROOM_CONNECT_TIMEOUT_MS 20000U
#define REMOTE_AGENT_JOIN_TIMEOUT_MS 30000U
#define CLOUD_RETRY_MS          30000U
#define AGENT_TASK_STACK_BYTES  8192U
#define UPLINK_TASK_STACK_BYTES 12288U
#define DOWNLINK_TASK_STACK_BYTES 8192U
#define STACK_WORDS(bytes)      ((bytes) / sizeof(StackType_t))

#define CONV_AI_CONFIG_FORMAT \
    "{\"ver\":1," \
    "\"iot\":{" \
        "\"instance_id\":\"%s\"," \
        "\"product_key\":\"%s\"," \
        "\"product_secret\":\"%s\"," \
        "\"device_name\":\"%s\"}," \
    "\"rtc\":{" \
        "\"log_level\":2," \
        "\"audio\":{\"publish\":true,\"subscribe\":true,\"codec\":3}," \
        "\"video\":{\"publish\":false,\"subscribe\":false,\"codec\":1}," \
        "\"params\":[" \
            "\"{\\\"debug\\\":{\\\"log_to_console\\\":1}}\"," \
            "\"{\\\"rtc\\\":{\\\"access\\\":{\\\"concurrent_requests\\\":1}}}\"," \
            "\"{\\\"rtc\\\":{\\\"ice\\\":{\\\"concurrent_agents\\\":1}}}\"," \
            "\"{\\\"rtc\\\":{\\\"network\\\":{\\\"enable_audio_jitter2\\\":0}}}\"," \
            "\"{\\\"rtc\\\":{\\\"report\\\":{\\\"enable\\\":0}}}\"," \
            "\"{\\\"audio\\\":{\\\"codec\\\":{\\\"pcma\\\":{\\\"s_samples_per_frame\\\":480}}}}\"" \
        "]}}"

typedef struct {
    uint16_t length;
    uint8_t data[DOWNLINK_PACKET_BYTES];
} downlink_packet_t;

typedef struct {
    EventGroupHandle_t events;
    StaticEventGroup_t events_ctrl;
    QueueHandle_t downlink_ready_queue;
    QueueHandle_t downlink_free_queue;
    StaticQueue_t downlink_ready_queue_ctrl;
    StaticQueue_t downlink_free_queue_ctrl;
    uint8_t downlink_ready_queue_storage[
        DOWNLINK_QUEUE_LEN * sizeof(downlink_packet_t *)];
    uint8_t downlink_free_queue_storage[
        DOWNLINK_QUEUE_LEN * sizeof(downlink_packet_t *)];
    recorder_sr_handle_t sr;
    recorder_sr_iface_t *sr_iface;
    void *g711_encoder;
    void *g711_decoder;
    volc_engine_t engine;
    volatile bool starting;
    volatile bool session_started;
    volatile bool room_connected;
    volatile bool remote_agent_joined;
    volatile bool cloud_ready;
    volatile bool listening_confirmed;
    volatile bool uplink_enabled;
    bool credentials_ready;
    bool pending_start;
    bool wake_ack_played;
    int64_t session_start_ms;
    int64_t room_join_ms;
    int64_t last_cloud_attempt_ms;
    volatile uint32_t uplink_packets;
    volatile uint32_t downlink_packets;
    volatile uint32_t downlink_drops;
    volatile uint32_t playback_drops;
    StaticTask_t agent_task_ctrl;
    StaticTask_t uplink_task_ctrl;
    StaticTask_t downlink_task_ctrl;
} rtc_agent_ctx_t;

static rtc_agent_ctx_t s_agent;
static StackType_t s_agent_task_stack[STACK_WORDS(AGENT_TASK_STACK_BYTES)];
static StackType_t s_uplink_task_stack[STACK_WORDS(UPLINK_TASK_STACK_BYTES)];
static StackType_t s_downlink_task_stack[STACK_WORDS(DOWNLINK_TASK_STACK_BYTES)];
EXT_RAM_BSS_ATTR static downlink_packet_t
    s_downlink_packets[DOWNLINK_QUEUE_LEN];
EXT_RAM_BSS_ATTR static uint8_t s_afe_pcm[AFE_FRAME_BYTES];
EXT_RAM_BSS_ATTR static int16_t s_uplink_pcm8k[G711_PACKET_SAMPLES];
EXT_RAM_BSS_ATTR static uint8_t s_uplink_g711[G711_PACKET_BYTES];
EXT_RAM_BSS_ATTR static int16_t s_downlink_pcm8k[DOWNLINK_PACKET_BYTES];
EXT_RAM_BSS_ATTR static int16_t s_playback_pcm16k[DOWNLINK_PACKET_BYTES * 2U];
EXT_RAM_BSS_ATTR static char s_cloud_config[1536];

extern const uint8_t wake_ack_wozai_pcm_start[]
    asm("_binary_wake_ack_wozai_pcm_start");
extern const uint8_t wake_ack_wozai_pcm_end[]
    asm("_binary_wake_ack_wozai_pcm_end");

static bool nonempty_config(const char *value)
{
    return value && value[0] != '\0' && strcmp(value, "xxxx") != 0;
}

static bool system_time_ready(void)
{
    time_t now = 0;
    time(&now);
    return now >= 1704067200;
}

static void play_wake_ack(void)
{
    const int16_t *samples = (const int16_t *)wake_ack_wozai_pcm_start;
    size_t sample_count =
        (size_t)(wake_ack_wozai_pcm_end - wake_ack_wozai_pcm_start) /
        sizeof(*samples);

    rtc_audio_flush_playback();
    while (sample_count > 0) {
        size_t chunk = sample_count > RTC_AUDIO_FRAME_SAMPLES
                           ? RTC_AUDIO_FRAME_SAMPLES
                           : sample_count;
        esp_err_t err = rtc_audio_play_pcm(samples, chunk,
                                           pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wake acknowledgement playback failed: %s",
                     esp_err_to_name(err));
            break;
        }
        samples += chunk;
        sample_count -= chunk;
    }
    vTaskDelay(pdMS_TO_TICKS(550));
}

static esp_err_t afe_monitor(recorder_sr_result_t *result, void *user_ctx)
{
    (void)user_ctx;
    if (result && result->type == SR_RESULT_WAKEUP &&
        !s_agent.starting && !s_agent.session_started) {
        ESP_LOGI(TAG, "wake phrase detected; requesting hardware-agent session");
        xEventGroupSetBits(s_agent.events, AGENT_BIT_WAKE);
    }
    return ESP_OK;
}

static void on_volc_event(volc_engine_t handle,
                          volc_event_t *event,
                          void *user_data)
{
    (void)handle;
    (void)user_data;
    if (!event) {
        return;
    }
    switch (event->code) {
    case VOLC_EV_CONNECTED:
        s_agent.room_connected = true;
        /* The upstream Embedded Kit starts capture before volc_start().
         * Start publishing as soon as the local RTC room is ready so the
         * cloud agent is not waiting for a first audio packet from us. */
        s_agent.uplink_enabled = true;
        xEventGroupSetBits(s_agent.events, AGENT_BIT_CONNECTED);
        break;
    case VOLC_EV_DISCONNECTED:
        s_agent.room_connected = false;
        s_agent.remote_agent_joined = false;
        s_agent.cloud_ready = false;
        s_agent.listening_confirmed = false;
        s_agent.uplink_enabled = false;
        xEventGroupSetBits(s_agent.events, AGENT_BIT_DISCONNECTED);
        break;
    case VOLC_EV_REMOTE_USER_JOINED:
        s_agent.remote_agent_joined = true;
        xEventGroupSetBits(s_agent.events, AGENT_BIT_REMOTE_JOINED);
        break;
    case VOLC_EV_REMOTE_USER_OFFLINE:
        s_agent.remote_agent_joined = false;
        s_agent.uplink_enabled = false;
        xEventGroupSetBits(s_agent.events, AGENT_BIT_REMOTE_LEFT);
        break;
    case VOLC_EV_TOKEN_EXPIRED:
        s_agent.uplink_enabled = false;
        xEventGroupSetBits(s_agent.events, AGENT_BIT_TOKEN_EXPIRED);
        break;
    case VOLC_EV_LICENSE_WARNING:
        xEventGroupSetBits(s_agent.events, AGENT_BIT_LICENSE_WARNING);
        break;
    case VOLC_EV_QUOTA_EXCEEDED:
        s_agent.remote_agent_joined = false;
        s_agent.cloud_ready = false;
        s_agent.listening_confirmed = false;
        s_agent.uplink_enabled = false;
        xEventGroupSetBits(s_agent.events, AGENT_BIT_QUOTA);
        break;
    default:
        ESP_LOGW(TAG, "unhandled hardware-agent event=%d", event->code);
        break;
    }
}

static void on_conversation_status(volc_engine_t handle,
                                   volc_conv_status_e status,
                                   void *user_data)
{
    (void)handle;
    (void)user_data;
    const char *name = "unknown";
    switch (status) {
    case VOLC_CONV_STATUS_LISTENING:
        name = "listening";
        s_agent.listening_confirmed = true;
        if (!s_agent.cloud_ready) {
            s_agent.cloud_ready = true;
        }
        xEventGroupSetBits(s_agent.events, AGENT_BIT_CLOUD_READY);
        break;
    case VOLC_CONV_STATUS_THINKING:
        name = "thinking";
        break;
    case VOLC_CONV_STATUS_ANSWERING:
        name = "answering";
        break;
    case VOLC_CONV_STATUS_INTERRUPTED:
        name = "interrupted";
        rtc_audio_flush_playback();
        break;
    case VOLC_CONV_STATUS_ANSWER_FINISH:
        name = "answer_finished";
        break;
    default:
        break;
    }
    ESP_LOGI(TAG, "conversation state=%s(%d)", name, status);
}

static void on_audio_data(volc_engine_t handle,
                          const void *data_ptr,
                          size_t data_len,
                          volc_audio_frame_info_t *info_ptr,
                          void *user_data)
{
    (void)handle;
    (void)user_data;
    if (!s_agent.room_connected || !data_ptr || data_len == 0 ||
        data_len > DOWNLINK_PACKET_BYTES) {
        return;
    }
    if (!info_ptr || info_ptr->data_type != VOLC_AUDIO_DATA_TYPE_G711A) {
        ESP_LOGW(TAG, "unexpected downlink audio type=%d bytes=%u",
                 info_ptr ? (int)info_ptr->data_type : -1,
                 (unsigned)data_len);
        return;
    }

    downlink_packet_t *packet = NULL;
    if (xQueueReceive(s_agent.downlink_free_queue, &packet, 0) != pdPASS ||
        !packet) {
        s_agent.downlink_drops++;
        return;
    }
    packet->length = (uint16_t)data_len;
    memcpy(packet->data, data_ptr, data_len);
    if (!s_agent.cloud_ready) {
        s_agent.cloud_ready = true;
        xEventGroupSetBits(s_agent.events, AGENT_BIT_CLOUD_READY);
        ESP_LOGI(TAG, "cloud agent confirmed by first downlink audio packet");
    }
    if (xQueueSend(s_agent.downlink_ready_queue, &packet, 0) != pdPASS) {
        s_agent.downlink_drops++;
        (void)xQueueSend(s_agent.downlink_free_queue, &packet, 0);
        return;
    }
    s_agent.downlink_packets++;
    if (s_agent.downlink_packets == 1U) {
        ESP_LOGI(TAG, "first hardware-agent downlink packet received bytes=%u",
                 (unsigned)data_len);
    }
}

static void on_message_data(volc_engine_t handle,
                            const void *data_ptr,
                            size_t data_len,
                            volc_message_info_t *info_ptr,
                            void *user_data)
{
    (void)handle;
    (void)user_data;
    rtc_message_process(data_ptr, data_len,
                        info_ptr ? info_ptr->is_binary : false);
}

static esp_err_t init_afe(void)
{
    recorder_sr_cfg_t config = DEFAULT_RECORDER_SR_CFG(
        "MR", "model", AFE_TYPE_SR, AFE_MODE_LOW_COST);
    ESP_RETURN_ON_FALSE(config.afe_cfg, ESP_ERR_NO_MEM, TAG,
                        "AFE config allocation failed");
    config.multinet_init = false;
    config.afe_cfg->aec_init = true;
    config.afe_cfg->se_init = true;
    config.afe_cfg->vad_init = false;
    config.afe_cfg->wakenet_init = true;
    config.afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    config.afe_cfg->agc_mode = AFE_MN_PEAK_NO_AGC;
    config.afe_cfg->pcm_config.mic_num = 1;
    config.afe_cfg->pcm_config.ref_num = 1;
    config.afe_cfg->pcm_config.total_ch_num = 2;
    config.afe_cfg->wakenet_mode = DET_MODE_95;

    s_agent.sr = recorder_sr_create(&config, &s_agent.sr_iface);
    ESP_RETURN_ON_FALSE(s_agent.sr && s_agent.sr_iface,
                        ESP_FAIL, TAG, "AFE creation failed");
    ESP_RETURN_ON_ERROR(
        s_agent.sr_iface->base.set_read_cb(
            s_agent.sr, rtc_audio_read_aec, NULL),
        TAG, "AFE input callback setup failed");
    ESP_RETURN_ON_ERROR(
        s_agent.sr_iface->set_afe_monitor(s_agent.sr, afe_monitor, NULL),
        TAG, "AFE monitor setup failed");
    ESP_RETURN_ON_ERROR(s_agent.sr_iface->base.enable(s_agent.sr, true),
                        TAG, "AFE tasks failed to start");
    ESP_LOGI(TAG, "AFE ready: WakeNet + NS + full-duplex AEC");
    return ESP_OK;
}

static esp_err_t init_g711(void)
{
    esp_g711_enc_config_t encoder_config = ESP_G711_ENC_CONFIG_DEFAULT();
    encoder_config.sample_rate = G711_SAMPLE_RATE_HZ;
    encoder_config.channel = 1;
    encoder_config.bits_per_sample = 16;
    encoder_config.frame_duration = G711_PACKET_MS;
    ESP_RETURN_ON_FALSE(
        esp_g711a_enc_open(&encoder_config, sizeof(encoder_config),
                           &s_agent.g711_encoder) == ESP_AUDIO_ERR_OK,
        ESP_FAIL, TAG, "G711A encoder init failed");

    esp_g711_dec_cfg_t decoder_config = ESP_G711_DEC_CONFIG_DEFAULT();
    ESP_RETURN_ON_FALSE(
        esp_g711_dec_open(&decoder_config, sizeof(decoder_config),
                          &s_agent.g711_decoder) == ESP_AUDIO_ERR_OK,
        ESP_FAIL, TAG, "G711A decoder init failed");
    ESP_LOGI(TAG, "G711A ready: 8kHz mono 60ms packets");
    return ESP_OK;
}

static void uplink_task(void *arg)
{
    (void)arg;
    size_t pcm8k_samples = 0;
    uint64_t sent_level_sum = 0;
    uint32_t sent_level_samples = 0;
    uint32_t sent_level_peak = 0;
    uint32_t sent_clipped_samples = 0;
    volc_audio_frame_info_t frame_info = {
        .data_type = VOLC_AUDIO_DATA_TYPE_G711A,
        .commit = false,
    };

    for (;;) {
        int received = s_agent.sr_iface->base.fetch(
            s_agent.sr, s_afe_pcm, sizeof(s_afe_pcm), portMAX_DELAY);
        if (received != (int)sizeof(s_afe_pcm)) {
            if (received < 0) {
                ESP_LOGW(TAG, "AFE fetch failed: %d", received);
            }
            continue;
        }
        if (!s_agent.uplink_enabled || !s_agent.engine) {
            pcm8k_samples = 0;
            continue;
        }

        const int16_t *pcm16k = (const int16_t *)s_afe_pcm;
        for (size_t i = 0; i < AFE_FRAME_SAMPLES; i += 2U) {
            int32_t mixed = (int32_t)pcm16k[i] + pcm16k[i + 1U];
            s_uplink_pcm8k[pcm8k_samples++] = (int16_t)(mixed / 2);
        }
        if (pcm8k_samples < G711_PACKET_SAMPLES) {
            continue;
        }

        uint64_t packet_level_sum = 0;
        uint32_t packet_level_peak = 0;
        uint32_t packet_clipped_samples = 0;
        for (size_t i = 0; i < G711_PACKET_SAMPLES; ++i) {
            int32_t sample = s_uplink_pcm8k[i];
            uint32_t magnitude =
                (uint32_t)(sample < 0 ? -sample : sample);
            packet_level_sum += magnitude;
            if (magnitude > packet_level_peak) {
                packet_level_peak = magnitude;
            }
            if (sample == INT16_MIN || sample == INT16_MAX) {
                packet_clipped_samples++;
            }
        }

        esp_audio_enc_in_frame_t input = {
            .buffer = (uint8_t *)s_uplink_pcm8k,
            .len = sizeof(s_uplink_pcm8k),
        };
        esp_audio_enc_out_frame_t output = {
            .buffer = s_uplink_g711,
            .len = sizeof(s_uplink_g711),
        };
        esp_audio_err_t encode_result = esp_g711_enc_process(
            s_agent.g711_encoder, &input, &output);
        pcm8k_samples = 0;
        if (encode_result != ESP_AUDIO_ERR_OK ||
            output.encoded_bytes != G711_PACKET_BYTES) {
            ESP_LOGW(TAG, "G711A encode failed code=%d bytes=%u",
                     (int)encode_result, (unsigned)output.encoded_bytes);
            continue;
        }

        int send_result = volc_send_audio_data(
            s_agent.engine, s_uplink_g711, output.encoded_bytes, &frame_info);
        if (send_result != 0) {
            ESP_LOGW(TAG, "hardware-agent uplink failed code=%d", send_result);
            continue;
        }
        s_agent.uplink_packets++;
        sent_level_sum += packet_level_sum;
        sent_level_samples += G711_PACKET_SAMPLES;
        if (packet_level_peak > sent_level_peak) {
            sent_level_peak = packet_level_peak;
        }
        sent_clipped_samples += packet_clipped_samples;
        if (s_agent.uplink_packets == 1U) {
            ESP_LOGI(TAG,
                     "first hardware-agent uplink packet sent mean=%u peak=%u clipped=%u",
                     (unsigned)(packet_level_sum / G711_PACKET_SAMPLES),
                     (unsigned)packet_level_peak,
                     (unsigned)packet_clipped_samples);
        } else if ((s_agent.uplink_packets % 500U) == 0U) {
            ESP_LOGI(TAG,
                     "audio health uplink=%u downlink=%u tx_mean=%u tx_peak=%u tx_clipped=%u rx_drops=%u playback_drops=%u",
                     (unsigned)s_agent.uplink_packets,
                     (unsigned)s_agent.downlink_packets,
                     sent_level_samples > 0
                         ? (unsigned)(sent_level_sum / sent_level_samples)
                         : 0U,
                     (unsigned)sent_level_peak,
                     (unsigned)sent_clipped_samples,
                     (unsigned)s_agent.downlink_drops,
                     (unsigned)s_agent.playback_drops);
            sent_level_sum = 0;
            sent_level_samples = 0;
            sent_level_peak = 0;
            sent_clipped_samples = 0;
        }
    }
}

static void downlink_task(void *arg)
{
    (void)arg;
    for (;;) {
        downlink_packet_t *packet = NULL;
        if (xQueueReceive(s_agent.downlink_ready_queue, &packet,
                          portMAX_DELAY) != pdPASS || !packet) {
            continue;
        }
        if (!s_agent.room_connected || packet->length == 0) {
            (void)xQueueSend(s_agent.downlink_free_queue, &packet, 0);
            continue;
        }

        esp_audio_dec_in_raw_t input = {
            .buffer = packet->data,
            .len = packet->length,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        esp_audio_dec_out_frame_t output = {
            .buffer = (uint8_t *)s_downlink_pcm8k,
            .len = sizeof(s_downlink_pcm8k),
        };
        esp_audio_dec_info_t info = {0};
        esp_audio_err_t decode_result = esp_g711a_dec_decode(
            s_agent.g711_decoder, &input, &output, &info);
        if (decode_result != ESP_AUDIO_ERR_OK || output.decoded_size == 0 ||
            (output.decoded_size % sizeof(int16_t)) != 0) {
            ESP_LOGW(TAG, "G711A decode failed code=%d bytes=%u",
                     (int)decode_result, (unsigned)output.decoded_size);
            (void)xQueueSend(s_agent.downlink_free_queue, &packet, 0);
            continue;
        }

        size_t samples8k = output.decoded_size / sizeof(int16_t);
        for (size_t i = 0; i < samples8k; ++i) {
            s_playback_pcm16k[i * 2U] = s_downlink_pcm8k[i];
            s_playback_pcm16k[i * 2U + 1U] = s_downlink_pcm8k[i];
        }
        esp_err_t play_result = rtc_audio_play_pcm(
            s_playback_pcm16k, samples8k * 2U, pdMS_TO_TICKS(100));
        if (play_result != ESP_OK) {
            s_agent.playback_drops++;
        }
        (void)xQueueSend(s_agent.downlink_free_queue, &packet, 0);
    }
}

static esp_err_t create_cloud_engine(void)
{
    int written = snprintf(
        s_cloud_config, sizeof(s_cloud_config), CONV_AI_CONFIG_FORMAT,
        CONFIG_RTC_AGENT_VOLC_INSTANCE_ID,
        CONFIG_RTC_AGENT_VOLC_PRODUCT_KEY,
        CONFIG_RTC_AGENT_VOLC_PRODUCT_SECRET,
        CONFIG_RTC_AGENT_VOLC_DEVICE_NAME);
    ESP_RETURN_ON_FALSE(written > 0 && written < (int)sizeof(s_cloud_config),
                        ESP_ERR_INVALID_SIZE, TAG,
                        "hardware-agent configuration is too large");

    volc_event_handler_t handlers = {
        .on_volc_event = on_volc_event,
        .on_volc_conversation_status = on_conversation_status,
        .on_volc_audio_data = on_audio_data,
        .on_volc_video_data = NULL,
        .on_volc_message_data = on_message_data,
    };
    int result = volc_create(&s_agent.engine, s_cloud_config,
                             &handlers, &s_agent);
    memset(s_cloud_config, 0, sizeof(s_cloud_config));
    if (result != 0 || !s_agent.engine) {
        s_agent.engine = NULL;
        ESP_LOGE(TAG, "hardware-agent registration failed code=%d (%s)",
                 result, volc_err_2_str(result));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "hardware-agent SDK ready version=%s device=%s",
             volc_get_version(), CONFIG_RTC_AGENT_VOLC_DEVICE_NAME);
    return ESP_OK;
}

static void stop_session(void)
{
    s_agent.uplink_enabled = false;
    s_agent.room_connected = false;
    s_agent.remote_agent_joined = false;
    s_agent.cloud_ready = false;
    s_agent.listening_confirmed = false;
    s_agent.starting = false;
    s_agent.pending_start = false;
    rtc_audio_flush_playback();
    if (s_agent.session_started && s_agent.engine) {
        int result = volc_stop(s_agent.engine);
        if (result != 0) {
            ESP_LOGW(TAG, "hardware-agent stop returned %d", result);
        }
    }
    s_agent.session_started = false;
    s_agent.session_start_ms = 0;
    s_agent.room_join_ms = 0;
    s_agent.wake_ack_played = false;
}

static void start_session(void)
{
    if (!s_agent.engine || s_agent.session_started || s_agent.starting) {
        return;
    }
    s_agent.starting = true;
    s_agent.wake_ack_played = false;
    s_agent.room_connected = false;
    s_agent.remote_agent_joined = false;
    s_agent.cloud_ready = false;
    s_agent.listening_confirmed = false;
    s_agent.uplink_enabled = false;
    s_agent.uplink_packets = 0;
    s_agent.downlink_packets = 0;
    s_agent.downlink_drops = 0;
    s_agent.playback_drops = 0;
    volc_opt_t options = {
        .mode = VOLC_MODE_RTC,
        .bot_id = CONFIG_RTC_AGENT_VOLC_BOT_ID,
        .params = NULL,
    };
    int result = volc_start(s_agent.engine, &options);
    if (result != 0) {
        ESP_LOGE(TAG, "hardware-agent session start failed code=%d (%s)",
                 result, volc_err_2_str(result));
        s_agent.starting = false;
        s_agent.pending_start = false;
        s_agent.session_start_ms = 0;
        s_agent.room_join_ms = 0;
        return;
    }
    s_agent.session_started = true;
    s_agent.session_start_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "hardware-agent session requested; waiting for RTC connection");
}

static void agent_task(void *arg)
{
    (void)arg;
#if CONFIG_RTC_AGENT_AUTO_START
    if (CONFIG_RTC_AGENT_AUTO_START) {
        s_agent.pending_start = true;
    }
#endif

    for (;;) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (!s_agent.engine && s_agent.credentials_ready &&
            system_time_ready() &&
            (s_agent.last_cloud_attempt_ms == 0 ||
             now_ms - s_agent.last_cloud_attempt_ms >= CLOUD_RETRY_MS)) {
            s_agent.last_cloud_attempt_ms = now_ms;
            if (create_cloud_engine() == ESP_OK && s_agent.pending_start) {
                start_session();
            }
        }

        EventBits_t bits = xEventGroupWaitBits(
            s_agent.events,
            AGENT_BIT_WAKE | AGENT_BIT_CONNECTED |
                AGENT_BIT_DISCONNECTED | AGENT_BIT_QUOTA |
                AGENT_BIT_REMOTE_JOINED | AGENT_BIT_REMOTE_LEFT |
                AGENT_BIT_TOKEN_EXPIRED | AGENT_BIT_LICENSE_WARNING |
                AGENT_BIT_CLOUD_READY,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));

        if (bits & AGENT_BIT_WAKE) {
            s_agent.pending_start = true;
            if (!s_agent.credentials_ready) {
                ESP_LOGE(TAG,
                         "hardware-agent credentials are incomplete; configure the five Volc fields");
                s_agent.pending_start = false;
            } else if (!system_time_ready()) {
                ESP_LOGW(TAG,
                         "system time is not synchronized; keeping wake request pending");
            } else if (s_agent.engine) {
                start_session();
            }
        }

        if (bits & AGENT_BIT_CONNECTED) {
            s_agent.pending_start = false;
            if (s_agent.room_join_ms == 0) {
                s_agent.room_join_ms = esp_timer_get_time() / 1000;
                ESP_LOGI(TAG,
                         "device joined RTC room; audio uplink active while waiting for cloud agent");
                s_agent.starting = false;
                if (s_agent.cloud_ready) {
                    xEventGroupSetBits(s_agent.events,
                                       AGENT_BIT_CLOUD_READY);
                }
            } else {
                ESP_LOGI(TAG, "duplicate local RTC connected event ignored");
            }
        }

        if (bits & AGENT_BIT_REMOTE_JOINED) {
            ESP_LOGI(TAG, "remote RTC user joined; cloud agent is available");
            /* Some RTC sessions deliver audio/subtitles without the optional
             * conversation-state callback. Do not hold the wake acknowledgement
             * or the conversation behind that callback. */
            s_agent.cloud_ready = true;
            s_agent.starting = false;
            if (!s_agent.wake_ack_played && s_agent.session_started &&
                s_agent.room_connected) {
                s_agent.wake_ack_played = true;
                s_agent.uplink_enabled = false;
                ESP_LOGI(TAG, "remote agent joined; playing 'wo zai'");
                play_wake_ack();
                if (s_agent.session_started && s_agent.room_connected) {
                    s_agent.uplink_enabled = true;
                    ESP_LOGI(TAG, "wake acknowledgement complete; speak now");
                }
            }
        }

        if ((bits & AGENT_BIT_CLOUD_READY) && s_agent.session_started &&
            s_agent.room_connected) {
            s_agent.starting = false;
            if (s_agent.listening_confirmed && !s_agent.wake_ack_played) {
                s_agent.wake_ack_played = true;
                s_agent.uplink_enabled = false;
                ESP_LOGI(TAG,
                         "official SDK LISTENING confirmed; playing 'wo zai'");
                play_wake_ack();
                if (s_agent.session_started && s_agent.room_connected &&
                    s_agent.cloud_ready) {
                    s_agent.uplink_enabled = true;
                    ESP_LOGI(TAG,
                             "wake acknowledgement complete; speak now");
                }
            }
            ESP_LOGI(TAG, "cloud agent ready; continuous conversation active");
        } else if ((bits & AGENT_BIT_CLOUD_READY) &&
                   !s_agent.session_started) {
            s_agent.cloud_ready = false;
            s_agent.listening_confirmed = false;
            ESP_LOGW(TAG, "ignoring stale cloud-ready event after session ended");
        } else if (bits & AGENT_BIT_CLOUD_READY) {
            ESP_LOGI(TAG,
                     "cloud-ready state arrived before local RTC join; deferring acknowledgement");
        }

        if ((bits & AGENT_BIT_REMOTE_LEFT) && s_agent.session_started) {
            ESP_LOGW(TAG, "cloud agent left RTC room; ending conversation");
            stop_session();
        }

        if (bits & AGENT_BIT_DISCONNECTED) {
            ESP_LOGW(TAG, "device disconnected from RTC room");
            stop_session();
        }
        if (bits & AGENT_BIT_TOKEN_EXPIRED) {
            ESP_LOGE(TAG, "hardware-agent RTC token expired");
            stop_session();
        }
        if (bits & AGENT_BIT_LICENSE_WARNING) {
            ESP_LOGW(TAG,
                     "hardware-agent License will expire; renew it in the Volc console");
        }
        if (bits & AGENT_BIT_QUOTA) {
            ESP_LOGE(TAG, "hardware-agent license quota is exhausted or expired");
            stop_session();
        }

        now_ms = esp_timer_get_time() / 1000;
        if (s_agent.session_started && !s_agent.room_connected &&
            s_agent.session_start_ms > 0 &&
            now_ms - s_agent.session_start_ms > RTC_ROOM_CONNECT_TIMEOUT_MS) {
            ESP_LOGE(TAG, "device RTC room connection timed out");
            stop_session();
        }
        if (s_agent.session_started && s_agent.room_connected &&
            !s_agent.remote_agent_joined && !s_agent.cloud_ready &&
            s_agent.room_join_ms > 0 &&
            now_ms - s_agent.room_join_ms > REMOTE_AGENT_JOIN_TIMEOUT_MS) {
            ESP_LOGE(TAG,
                     "cloud Bot did not join or send audio after 30s (uplink_packets=%u); verify product/Bot association and cloud task status",
                     (unsigned)s_agent.uplink_packets);
            stop_session();
        }
        if (s_agent.session_started && s_agent.session_start_ms > 0 &&
            now_ms - s_agent.session_start_ms >
                (int64_t)CONFIG_RTC_AGENT_SESSION_MAX_SECONDS * 1000) {
            ESP_LOGI(TAG, "hardware-agent session reached configured limit");
            stop_session();
        }
    }
}

static esp_err_t init_downlink_queues(void)
{
    s_agent.downlink_ready_queue = xQueueCreateStatic(
        DOWNLINK_QUEUE_LEN, sizeof(downlink_packet_t *),
        s_agent.downlink_ready_queue_storage,
        &s_agent.downlink_ready_queue_ctrl);
    s_agent.downlink_free_queue = xQueueCreateStatic(
        DOWNLINK_QUEUE_LEN, sizeof(downlink_packet_t *),
        s_agent.downlink_free_queue_storage,
        &s_agent.downlink_free_queue_ctrl);
    ESP_RETURN_ON_FALSE(s_agent.downlink_ready_queue &&
                            s_agent.downlink_free_queue,
                        ESP_ERR_NO_MEM, TAG,
                        "downlink queues failed to initialize");
    for (size_t i = 0; i < DOWNLINK_QUEUE_LEN; ++i) {
        downlink_packet_t *packet = &s_downlink_packets[i];
        ESP_RETURN_ON_FALSE(
            xQueueSend(s_agent.downlink_free_queue, &packet, 0) == pdPASS,
            ESP_FAIL, TAG, "downlink packet pool init failed");
    }
    return ESP_OK;
}

esp_err_t rtc_agent_start(void)
{
    memset(&s_agent, 0, sizeof(s_agent));
    s_agent.events = xEventGroupCreateStatic(&s_agent.events_ctrl);
    ESP_RETURN_ON_FALSE(s_agent.events, ESP_ERR_NO_MEM, TAG,
                        "agent event group init failed");
    ESP_RETURN_ON_ERROR(init_downlink_queues(), TAG,
                        "downlink queue init failed");
    ESP_RETURN_ON_ERROR(rtc_message_start(), TAG,
                        "message worker init failed");
    ESP_RETURN_ON_ERROR(rtc_audio_start(), TAG,
                        "full-duplex audio init failed");
    ESP_RETURN_ON_ERROR(init_afe(), TAG, "AFE init failed");
    ESP_RETURN_ON_ERROR(init_g711(), TAG, "G711A init failed");

    s_agent.credentials_ready =
        nonempty_config(CONFIG_RTC_AGENT_VOLC_INSTANCE_ID) &&
        nonempty_config(CONFIG_RTC_AGENT_VOLC_PRODUCT_KEY) &&
        nonempty_config(CONFIG_RTC_AGENT_VOLC_PRODUCT_SECRET) &&
        nonempty_config(CONFIG_RTC_AGENT_VOLC_DEVICE_NAME) &&
        nonempty_config(CONFIG_RTC_AGENT_VOLC_BOT_ID);

    TaskHandle_t uplink = xTaskCreateStaticPinnedToCore(
        uplink_task, "rtc_uplink", STACK_WORDS(UPLINK_TASK_STACK_BYTES),
        NULL, 10, s_uplink_task_stack, &s_agent.uplink_task_ctrl, 0);
    TaskHandle_t downlink = xTaskCreateStaticPinnedToCore(
        downlink_task, "rtc_downlink", STACK_WORDS(DOWNLINK_TASK_STACK_BYTES),
        NULL, 11, s_downlink_task_stack, &s_agent.downlink_task_ctrl, 0);
    TaskHandle_t agent = xTaskCreateStaticPinnedToCore(
        agent_task, "rtc_agent", STACK_WORDS(AGENT_TASK_STACK_BYTES),
        NULL, 7, s_agent_task_stack, &s_agent.agent_task_ctrl, 1);
    ESP_RETURN_ON_FALSE(uplink && downlink && agent,
                        ESP_ERR_NO_MEM, TAG, "agent task creation failed");

    ESP_LOGI(TAG,
             "ConversationalAI Embedded Kit 2.0 path ready codec=G711A cloud_config=%s",
             s_agent.credentials_ready ? "complete" : "incomplete");
    ESP_LOGI(TAG, "LISTENING: say '你好小益'");
    return ESP_OK;
}
