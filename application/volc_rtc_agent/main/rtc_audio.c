/* SPDX-License-Identifier: Apache-2.0 */

#include "rtc_audio.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "rtc_audio";

#define RTC_AUDIO_RAW_SLOTS               2U
#define RTC_AUDIO_RAW_FRAME_BYTES         (RTC_AUDIO_RAW_SLOTS * sizeof(int32_t))
#define RTC_AUDIO_BLOCK_BYTES             (RTC_AUDIO_FRAME_SAMPLES * sizeof(int16_t))
#define RTC_AUDIO_AEC_BLOCK_BYTES         (RTC_AUDIO_BLOCK_BYTES * 2U)
#define RTC_AUDIO_PLAYBACK_BUFFER_BYTES   (RTC_AUDIO_BLOCK_BYTES * 40U)
#define RTC_AUDIO_CAPTURE_BUFFER_BYTES    (RTC_AUDIO_AEC_BLOCK_BYTES * 20U)
#define RTC_AUDIO_MAX_REF_DELAY_BLOCKS    20U
#define RTC_AUDIO_DMA_DESC_NUM            3U
#define RTC_AUDIO_DMA_FRAME_NUM           48U
#define RTC_AUDIO_TX_TASK_STACK_BYTES     4096U
#define RTC_AUDIO_RX_TASK_STACK_BYTES     6144U
#define RTC_AUDIO_TX_TASK_STACK_WORDS     (RTC_AUDIO_TX_TASK_STACK_BYTES / sizeof(StackType_t))
#define RTC_AUDIO_RX_TASK_STACK_WORDS     (RTC_AUDIO_RX_TASK_STACK_BYTES / sizeof(StackType_t))
#define RTC_AUDIO_LEVEL_LOG_BLOCKS        (30000U / RTC_AUDIO_FRAME_MS)

typedef struct {
    i2s_chan_handle_t tx;
    i2s_chan_handle_t rx;
    StreamBufferHandle_t playback;
    StreamBufferHandle_t capture;
    StaticStreamBuffer_t playback_ctrl;
    StaticStreamBuffer_t capture_ctrl;
    StaticTask_t tx_task_ctrl;
    StaticTask_t rx_task_ctrl;
    TaskHandle_t tx_task;
    TaskHandle_t rx_task;
    volatile bool running;
    bool initialized;
    uint32_t ref_frames_written;
    uint32_t capture_drops;
    uint32_t playback_underruns;
} rtc_audio_ctx_t;

static rtc_audio_ctx_t s_audio;

EXT_RAM_BSS_ATTR static uint8_t s_playback_storage[RTC_AUDIO_PLAYBACK_BUFFER_BYTES];
EXT_RAM_BSS_ATTR static uint8_t s_capture_storage[RTC_AUDIO_CAPTURE_BUFFER_BYTES];
EXT_RAM_BSS_ATTR static int16_t
    s_ref_history[RTC_AUDIO_MAX_REF_DELAY_BLOCKS + 1U][RTC_AUDIO_FRAME_SAMPLES];
static StackType_t s_tx_task_stack[RTC_AUDIO_TX_TASK_STACK_WORDS];
static StackType_t s_rx_task_stack[RTC_AUDIO_RX_TASK_STACK_WORDS];
EXT_RAM_BSS_ATTR static int16_t s_io_speaker[RTC_AUDIO_FRAME_SAMPLES];
EXT_RAM_BSS_ATTR static int16_t s_io_aec[RTC_AUDIO_FRAME_SAMPLES * 2U];
EXT_RAM_BSS_ATTR static int32_t
    s_io_tx_raw[RTC_AUDIO_FRAME_SAMPLES * RTC_AUDIO_RAW_SLOTS];
EXT_RAM_BSS_ATTR static int32_t
    s_io_rx_raw[RTC_AUDIO_FRAME_SAMPLES * RTC_AUDIO_RAW_SLOTS];

static int16_t saturate_i16(int32_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static int16_t mic_raw_to_pcm(int32_t raw)
{
    const int32_t pcm = (raw >> 16) * CONFIG_RTC_AGENT_MIC_GAIN;
    return saturate_i16(pcm);
}

static int16_t apply_speaker_gain(int16_t sample)
{
    const int32_t scaled =
        ((int32_t)sample * CONFIG_RTC_AGENT_SPEAKER_GAIN_PERCENT) / 100;
    return saturate_i16(scaled);
}

static int32_t pcm_to_raw(int16_t sample)
{
    return (int32_t)sample * 65536;
}

static esp_err_t write_exact(const void *data, size_t bytes)
{
    size_t offset = 0;
    while (offset < bytes) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(
            s_audio.tx, (const uint8_t *)data + offset, bytes - offset,
            &written, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            return err;
        }
        if (written == 0) {
            return ESP_ERR_TIMEOUT;
        }
        offset += written;
    }
    return ESP_OK;
}

static esp_err_t read_exact(void *data, size_t bytes)
{
    size_t offset = 0;
    while (offset < bytes) {
        size_t received = 0;
        esp_err_t err = i2s_channel_read(
            s_audio.rx, (uint8_t *)data + offset, bytes - offset,
            &received, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            return err;
        }
        if (received == 0) {
            return ESP_ERR_TIMEOUT;
        }
        offset += received;
    }
    return ESP_OK;
}

static void audio_tx_task(void *arg)
{
    (void)arg;
    const size_t raw_bytes = sizeof(s_io_tx_raw);
    const size_t ref_slots = RTC_AUDIO_MAX_REF_DELAY_BLOCKS + 1U;

    while (s_audio.running) {
        memset(s_io_speaker, 0, sizeof(s_io_speaker));
        size_t got = xStreamBufferReceive(
            s_audio.playback, s_io_speaker, sizeof(s_io_speaker), 0);
        if (got > 0 && got < sizeof(s_io_speaker)) {
            memset((uint8_t *)s_io_speaker + got, 0,
                   sizeof(s_io_speaker) - got);
            s_audio.playback_underruns++;
        }

        for (size_t i = 0; i < RTC_AUDIO_FRAME_SAMPLES; ++i) {
            s_io_speaker[i] = apply_speaker_gain(s_io_speaker[i]);
            int32_t raw = pcm_to_raw(s_io_speaker[i]);
            s_io_tx_raw[i * 2U] = raw;
            s_io_tx_raw[i * 2U + 1U] = raw;
        }

        uint32_t ref_sequence = __atomic_load_n(
            &s_audio.ref_frames_written, __ATOMIC_RELAXED);
        memcpy(s_ref_history[ref_sequence % ref_slots], s_io_speaker,
               sizeof(s_io_speaker));
        __atomic_store_n(&s_audio.ref_frames_written, ref_sequence + 1U,
                         __ATOMIC_RELEASE);

        esp_err_t err = write_exact(s_io_tx_raw, raw_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S playback failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    s_audio.tx_task = NULL;
    vTaskDelete(NULL);
}

static void audio_rx_task(void *arg)
{
    (void)arg;
    const size_t ref_slots = RTC_AUDIO_MAX_REF_DELAY_BLOCKS + 1U;
    size_t delay_blocks =
        CONFIG_RTC_AGENT_AEC_REFERENCE_DELAY_MS / RTC_AUDIO_FRAME_MS;
    uint64_t slot_level_sum[RTC_AUDIO_RAW_SLOTS] = {0};
    uint32_t slot_level_peak[RTC_AUDIO_RAW_SLOTS] = {0};
    uint32_t slot_raw_or[RTC_AUDIO_RAW_SLOTS] = {0};
    uint32_t slot_raw_nonzero[RTC_AUDIO_RAW_SLOTS] = {0};
    uint32_t level_blocks = 0;
    if (delay_blocks > RTC_AUDIO_MAX_REF_DELAY_BLOCKS) {
        delay_blocks = RTC_AUDIO_MAX_REF_DELAY_BLOCKS;
    }

    ESP_LOGI(TAG,
             "full-duplex I2S running frame=%ums ref_delay=%ums mic_gain=%d speaker_gain=%d%%",
             (unsigned)RTC_AUDIO_FRAME_MS,
             (unsigned)(delay_blocks * RTC_AUDIO_FRAME_MS),
             CONFIG_RTC_AGENT_MIC_GAIN,
             CONFIG_RTC_AGENT_SPEAKER_GAIN_PERCENT);

    while (s_audio.running) {
        esp_err_t err = read_exact(s_io_rx_raw, sizeof(s_io_rx_raw));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S capture failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        const uint32_t ref_sequence = __atomic_load_n(
            &s_audio.ref_frames_written, __ATOMIC_ACQUIRE);
        const int16_t *reference = NULL;
        if (ref_sequence > delay_blocks) {
            const uint32_t ref_index =
                ref_sequence - 1U - (uint32_t)delay_blocks;
            reference = s_ref_history[ref_index % ref_slots];
        }
        for (size_t i = 0; i < RTC_AUDIO_FRAME_SAMPLES; ++i) {
#if CONFIG_RTC_AGENT_MIC_RIGHT_SLOT
            const size_t mic_slot = 1U;
#else
            const size_t mic_slot = 0U;
#endif
            int16_t slot_pcm[RTC_AUDIO_RAW_SLOTS];
            for (size_t slot = 0; slot < RTC_AUDIO_RAW_SLOTS; ++slot) {
                int32_t raw =
                    s_io_rx_raw[i * RTC_AUDIO_RAW_SLOTS + slot];
                slot_raw_or[slot] |= (uint32_t)raw;
                if (raw != 0) {
                    slot_raw_nonzero[slot]++;
                }
                slot_pcm[slot] = mic_raw_to_pcm(raw);
                int32_t sample = slot_pcm[slot];
                uint32_t magnitude =
                    (uint32_t)(sample < 0 ? -sample : sample);
                slot_level_sum[slot] += magnitude;
                if (magnitude > slot_level_peak[slot]) {
                    slot_level_peak[slot] = magnitude;
                }
            }
            s_io_aec[i * 2U] = slot_pcm[mic_slot];
            s_io_aec[i * 2U + 1U] = reference ? reference[i] : 0;
        }

        level_blocks++;
        if (level_blocks >= RTC_AUDIO_LEVEL_LOG_BLOCKS) {
            const uint32_t level_samples =
                level_blocks * RTC_AUDIO_FRAME_SAMPLES;
            ESP_LOGI(TAG,
                     "I2S mic health left_mean=%u left_peak=%u "
                     "left_raw_nz=%u left_raw_or=0x%08x "
                     "right_mean=%u right_peak=%u "
                     "right_raw_nz=%u right_raw_or=0x%08x selected=%s "
                     "capture_drops=%u playback_underruns=%u",
                     (unsigned)(slot_level_sum[0] / level_samples),
                     (unsigned)slot_level_peak[0],
                     (unsigned)slot_raw_nonzero[0],
                     (unsigned)slot_raw_or[0],
                     (unsigned)(slot_level_sum[1] / level_samples),
                     (unsigned)slot_level_peak[1],
                     (unsigned)slot_raw_nonzero[1],
                     (unsigned)slot_raw_or[1],
#if CONFIG_RTC_AGENT_MIC_RIGHT_SLOT
                     "right",
#else
                     "left",
#endif
                     (unsigned)s_audio.capture_drops,
                     (unsigned)s_audio.playback_underruns);
            memset(slot_level_sum, 0, sizeof(slot_level_sum));
            memset(slot_level_peak, 0, sizeof(slot_level_peak));
            memset(slot_raw_or, 0, sizeof(slot_raw_or));
            memset(slot_raw_nonzero, 0, sizeof(slot_raw_nonzero));
            level_blocks = 0;
        }

        if (xStreamBufferSpacesAvailable(s_audio.capture) >= sizeof(s_io_aec)) {
            size_t sent = xStreamBufferSend(
                s_audio.capture, s_io_aec, sizeof(s_io_aec), 0);
            if (sent != sizeof(s_io_aec)) {
                s_audio.capture_drops++;
            }
        } else {
            s_audio.capture_drops++;
            if ((s_audio.capture_drops % 100U) == 1U) {
                ESP_LOGW(TAG, "AFE input backpressure drops=%u",
                         (unsigned)s_audio.capture_drops);
            }
        }
    }

    s_audio.rx_task = NULL;
    vTaskDelete(NULL);
}

static void delete_channels(void)
{
    if (s_audio.rx) {
        (void)i2s_channel_disable(s_audio.rx);
        (void)i2s_del_channel(s_audio.rx);
        s_audio.rx = NULL;
    }
    if (s_audio.tx) {
        (void)i2s_channel_disable(s_audio.tx);
        (void)i2s_del_channel(s_audio.tx);
        s_audio.tx = NULL;
    }
}

esp_err_t rtc_audio_start(void)
{
    if (s_audio.initialized) {
        return ESP_OK;
    }

    memset(&s_audio, 0, sizeof(s_audio));
    s_audio.playback = xStreamBufferCreateStatic(
        sizeof(s_playback_storage), RTC_AUDIO_BLOCK_BYTES,
        s_playback_storage, &s_audio.playback_ctrl);
    s_audio.capture = xStreamBufferCreateStatic(
        sizeof(s_capture_storage), RTC_AUDIO_AEC_BLOCK_BYTES,
        s_capture_storage, &s_audio.capture_ctrl);
    ESP_RETURN_ON_FALSE(s_audio.playback && s_audio.capture,
                        ESP_ERR_NO_MEM, TAG, "static stream buffer init failed");

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = RTC_AUDIO_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = RTC_AUDIO_DMA_FRAME_NUM;
    chan_cfg.auto_clear_after_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_audio.tx, &s_audio.rx),
                        TAG, "I2S channel pair allocation failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RTC_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_RTC_AGENT_I2S_BCLK_GPIO,
            .ws = CONFIG_RTC_AGENT_I2S_WS_GPIO,
            .dout = CONFIG_RTC_AGENT_I2S_SPK_GPIO,
            .din = CONFIG_RTC_AGENT_I2S_MIC_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.ws_width = 32;

    esp_err_t err = i2s_channel_init_std_mode(s_audio.tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_init_std_mode(s_audio.rx, &std_cfg);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_audio.tx);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_audio.rx);
    }
    if (err != ESP_OK) {
        delete_channels();
        return err;
    }

    memset(s_ref_history, 0, sizeof(s_ref_history));
    s_audio.running = true;
    s_audio.tx_task = xTaskCreateStaticPinnedToCore(
        audio_tx_task, "rtc_audio_tx", RTC_AUDIO_TX_TASK_STACK_WORDS,
        NULL, 12, s_tx_task_stack, &s_audio.tx_task_ctrl, 0);
    s_audio.rx_task = xTaskCreateStaticPinnedToCore(
        audio_rx_task, "rtc_audio_rx", RTC_AUDIO_RX_TASK_STACK_WORDS,
        NULL, 13, s_rx_task_stack, &s_audio.rx_task_ctrl, 0);
    if (!s_audio.tx_task || !s_audio.rx_task) {
        s_audio.running = false;
        for (int i = 0; i < 100 &&
                        (s_audio.tx_task || s_audio.rx_task); ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        delete_channels();
        return ESP_ERR_NO_MEM;
    }

    s_audio.initialized = true;
    ESP_LOGI(TAG,
             "ready I2S0 16kHz 32-bit stereo bclk=%d ws=%d mic=%d spk=%d",
             CONFIG_RTC_AGENT_I2S_BCLK_GPIO,
             CONFIG_RTC_AGENT_I2S_WS_GPIO,
             CONFIG_RTC_AGENT_I2S_MIC_GPIO,
             CONFIG_RTC_AGENT_I2S_SPK_GPIO);
    return ESP_OK;
}

esp_err_t rtc_audio_stop(void)
{
    if (!s_audio.initialized) {
        return ESP_OK;
    }
    s_audio.running = false;
    for (int i = 0; i < 100 &&
                    (s_audio.tx_task || s_audio.rx_task); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_RETURN_ON_FALSE(!s_audio.tx_task && !s_audio.rx_task,
                        ESP_ERR_TIMEOUT, TAG,
                        "audio tasks did not stop");
    delete_channels();
    s_audio.initialized = false;
    return ESP_OK;
}

int rtc_audio_read_aec(void *buffer, int size, void *user_ctx, TickType_t ticks)
{
    (void)user_ctx;
    if (!buffer || size <= 0 || !s_audio.capture) {
        return 0;
    }

    size_t total = 0;
    while (total < (size_t)size && s_audio.running) {
        size_t got = xStreamBufferReceive(
            s_audio.capture, (uint8_t *)buffer + total,
            (size_t)size - total, ticks);
        if (got == 0) {
            break;
        }
        total += got;
    }
    return (int)total;
}

esp_err_t rtc_audio_play_pcm(const int16_t *samples,
                             size_t sample_count,
                             TickType_t ticks)
{
    ESP_RETURN_ON_FALSE(samples && sample_count > 0 && s_audio.playback,
                        ESP_ERR_INVALID_ARG, TAG, "invalid playback frame");
    const size_t bytes = sample_count * sizeof(*samples);
    ESP_RETURN_ON_FALSE(bytes <= sizeof(s_playback_storage),
                        ESP_ERR_INVALID_SIZE, TAG, "playback frame too large");

    /* Stream buffers may accept a partial write when the ring is nearly full.
     * Keep sending until the whole frame is queued so a short acknowledgement
     * or a decoded network packet is never truncated at the buffer boundary. */
    size_t total = 0;
    const TickType_t started = xTaskGetTickCount();
    TickType_t remaining = ticks;
    while (total < bytes) {
        size_t sent = xStreamBufferSend(
            s_audio.playback,
            (const uint8_t *)samples + total,
            bytes - total,
            remaining);
        if (sent == 0) {
            return ESP_ERR_TIMEOUT;
        }
        total += sent;
        if (ticks != portMAX_DELAY) {
            const TickType_t elapsed = xTaskGetTickCount() - started;
            if (elapsed >= ticks) {
                return total == bytes ? ESP_OK : ESP_ERR_TIMEOUT;
            }
            remaining = ticks - elapsed;
        }
    }
    return ESP_OK;
}

void rtc_audio_flush_playback(void)
{
    if (s_audio.playback) {
        (void)xStreamBufferReset(s_audio.playback);
    }
}
