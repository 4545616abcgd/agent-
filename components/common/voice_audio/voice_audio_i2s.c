/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voice_audio.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "voice_audio";

#define VOICE_AUDIO_IO_FRAMES          128U
/* V2.4: reserve DMA before WakeNet and trim each descriptor to 3 ms.
 * 3 x 48 frames gives ~9 ms/channel at 16 kHz: enough for the 8 ms writer
 * slices while reducing contiguous internal-DMA pressure by 25%. */
#define VOICE_AUDIO_DMA_DESC_NUM        3U
#define VOICE_AUDIO_DMA_FRAME_NUM       48U
#define VOICE_AUDIO_STEREO_SLOTS       2U
#define VOICE_AUDIO_SLOT_BYTES         sizeof(int32_t)
#define VOICE_AUDIO_FRAME_BYTES        (VOICE_AUDIO_STEREO_SLOTS * VOICE_AUDIO_SLOT_BYTES)
#define VOICE_AUDIO_PCM_BYTES          sizeof(int16_t)
#define VOICE_AUDIO_START_SILENCE_MS    50U
#define VOICE_AUDIO_TAIL_SILENCE_MS     50U
#define VOICE_AUDIO_DMA_DRAIN_MS        12U
#define VOICE_AUDIO_DEFAULT_TIMEOUT_MS 1000U

typedef struct {
    i2s_chan_handle_t tx;
    i2s_chan_handle_t rx;
    int32_t *rx_raw;
    int32_t *tx_raw;
    volatile voice_audio_state_t state;
    volatile bool initialized;
    volatile bool initializing;
    /*
     * State changes include blocking ESP-IDF driver calls and therefore
     * cannot run inside s_state_mux.  Reserve the transition atomically, do
     * the driver work outside the critical section, then publish completion.
     */
    bool transitioning;
    /* rx_raw/tx_raw are shared staging buffers: allow one I/O call at a time. */
    bool io_active;
} voice_audio_ctx_t;

static voice_audio_ctx_t s_audio;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static void log_audio_heap(const char *where)
{
    ESP_LOGI(TAG,
             "%s: internal_free=%u largest=%u dma_free=%u dma_largest=%u",
             where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
}

static bool try_begin_transition(voice_audio_state_t expected,
                                 voice_audio_state_t reserved_state)
{
    bool claimed = false;

    portENTER_CRITICAL(&s_state_mux);
    if (s_audio.initialized && !s_audio.initializing &&
        !s_audio.transitioning && !s_audio.io_active &&
        s_audio.state == expected) {
        s_audio.state = reserved_state;
        s_audio.transitioning = true;
        claimed = true;
    }
    portEXIT_CRITICAL(&s_state_mux);

    return claimed;
}

static void finish_transition(voice_audio_state_t state)
{
    portENTER_CRITICAL(&s_state_mux);
    s_audio.state = state;
    s_audio.transitioning = false;
    portEXIT_CRITICAL(&s_state_mux);
}

static bool try_begin_io(voice_audio_state_t expected)
{
    bool claimed = false;

    portENTER_CRITICAL(&s_state_mux);
    if (s_audio.initialized && !s_audio.initializing &&
        !s_audio.transitioning && !s_audio.io_active &&
        s_audio.state == expected) {
        s_audio.io_active = true;
        claimed = true;
    }
    portEXIT_CRITICAL(&s_state_mux);

    return claimed;
}

static void finish_io(void)
{
    portENTER_CRITICAL(&s_state_mux);
    s_audio.io_active = false;
    portEXIT_CRITICAL(&s_state_mux);
}

#define VOICE_AUDIO_MIC_GAIN_X 16

static int16_t raw_i2s_to_pcm16(int32_t raw)
{
    /*
     * INMP441 is 24-bit two's-complement I2S. With a 32-bit Philips slot the
     * valid sample occupies the most-significant 24 bits. Keep the upper
     * 16 bits, then apply a controlled digital microphone gain.
     *
     * Start with 16x for the current hardware because the measured capture
     * level was only peak=160, rms=18. Saturation prevents int16 overflow.
     */
    int32_t sample = raw >> 16;
    sample *= VOICE_AUDIO_MIC_GAIN_X;

    if (sample > INT16_MAX) {
        sample = INT16_MAX;
    } else if (sample < INT16_MIN) {
        sample = INT16_MIN;
    }

    return (int16_t)sample;
}

static int32_t pcm16_to_i2s(int16_t sample)
{
    /*
     * Place signed 16-bit PCM in the most-significant 16 bits of a 32-bit I2S
     * slot. Multiplication is used instead of left-shifting a negative signed
     * value (which would be undefined C behaviour).
     */
    return (int32_t)sample * 65536;
}

static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xffU);
    dst[1] = (uint8_t)((value >> 8) & 0xffU);
}

static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xffU);
    dst[1] = (uint8_t)((value >> 8) & 0xffU);
    dst[2] = (uint8_t)((value >> 16) & 0xffU);
    dst[3] = (uint8_t)((value >> 24) & 0xffU);
}

static uint16_t get_le16(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static esp_err_t wav_write_header(FILE *fp, uint32_t data_bytes)
{
    uint8_t header[44] = {0};
    const uint32_t sample_rate = CONFIG_VOICE_AUDIO_SAMPLE_RATE;
    const uint16_t channels = 1;
    const uint16_t bits_per_sample = 16;
    const uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8U);
    const uint16_t block_align = channels * (bits_per_sample / 8U);

    memcpy(&header[0], "RIFF", 4);
    put_le32(&header[4], 36U + data_bytes);
    memcpy(&header[8], "WAVE", 4);

    memcpy(&header[12], "fmt ", 4);
    put_le32(&header[16], 16U);
    put_le16(&header[20], 1U); /* PCM */
    put_le16(&header[22], channels);
    put_le32(&header[24], sample_rate);
    put_le32(&header[28], byte_rate);
    put_le16(&header[32], block_align);
    put_le16(&header[34], bits_per_sample);

    memcpy(&header[36], "data", 4);
    put_le32(&header[40], data_bytes);

    ESP_RETURN_ON_FALSE(fseek(fp, 0, SEEK_SET) == 0, ESP_FAIL, TAG,
                        "WAV seek failed: %s", strerror(errno));
    ESP_RETURN_ON_FALSE(fwrite(header, 1, sizeof(header), fp) == sizeof(header),
                        ESP_FAIL, TAG, "WAV header write failed: %s", strerror(errno));
    return ESP_OK;
}

static void audio_release_resources(void)
{
    if (s_audio.tx) {
        (void)i2s_del_channel(s_audio.tx);
        s_audio.tx = NULL;
    }
    if (s_audio.rx) {
        (void)i2s_del_channel(s_audio.rx);
        s_audio.rx = NULL;
    }
    free(s_audio.rx_raw);
    s_audio.rx_raw = NULL;
    free(s_audio.tx_raw);
    s_audio.tx_raw = NULL;
}

esp_err_t voice_audio_init(void)
{
    esp_err_t ret = ESP_OK;

    portENTER_CRITICAL(&s_state_mux);
    if (s_audio.initialized) {
        portEXIT_CRITICAL(&s_state_mux);
        return ESP_OK;
    }
    if (s_audio.initializing) {
        portEXIT_CRITICAL(&s_state_mux);
        return ESP_ERR_INVALID_STATE;
    }
    s_audio.initializing = true;
    portEXIT_CRITICAL(&s_state_mux);

    log_audio_heap("before I2S staging");

    /*
     * rx_raw / tx_raw are CPU-side staging buffers, not DMA buffers.
     * Put them in PSRAM so scarce contiguous internal/DMA RAM remains
     * available for the ESP-IDF I2S driver's TX/RX DMA rings.
     */
    s_audio.rx_raw = heap_caps_malloc(
        VOICE_AUDIO_IO_FRAMES * VOICE_AUDIO_FRAME_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_audio.tx_raw = heap_caps_malloc(
        VOICE_AUDIO_IO_FRAMES * VOICE_AUDIO_FRAME_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_GOTO_ON_FALSE(s_audio.rx_raw && s_audio.tx_raw, ESP_ERR_NO_MEM, fail, TAG,
                      "Failed to allocate PSRAM I2S staging buffers");
    ESP_LOGI(TAG,
             "I2S staging buffers allocated in PSRAM: %u bytes x2",
             (unsigned)(VOICE_AUDIO_IO_FRAMES * VOICE_AUDIO_FRAME_BYTES));
    log_audio_heap("after I2S staging");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = VOICE_AUDIO_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = VOICE_AUDIO_DMA_FRAME_NUM;
    /* A temporarily empty streaming ring must produce silence. With the ESP-IDF
     * default (false), TX DMA repeats the last descriptor indefinitely, which
     * turns the final vowel of a cloud chunk into a sustained hum. */
    chan_cfg.auto_clear_after_cb = true;

    ESP_LOGI(TAG, "I2S DMA config: desc=%u frame_num=%u bytes_per_desc=%u bytes_per_channel=%u",
             (unsigned)VOICE_AUDIO_DMA_DESC_NUM,
             (unsigned)VOICE_AUDIO_DMA_FRAME_NUM,
             (unsigned)(VOICE_AUDIO_DMA_FRAME_NUM * VOICE_AUDIO_FRAME_BYTES),
             (unsigned)(VOICE_AUDIO_DMA_DESC_NUM * VOICE_AUDIO_DMA_FRAME_NUM * VOICE_AUDIO_FRAME_BYTES));
    ESP_GOTO_ON_ERROR(i2s_new_channel(&chan_cfg, &s_audio.tx, &s_audio.rx),
                      fail, TAG, "Failed to allocate I2S TX/RX pair");
    log_audio_heap("after I2S channel pair");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_VOICE_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_VOICE_AUDIO_BCLK_GPIO,
            .ws = CONFIG_VOICE_AUDIO_WS_GPIO,
            .dout = CONFIG_VOICE_AUDIO_SPK_DATA_GPIO,
            .din = CONFIG_VOICE_AUDIO_MIC_DATA_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    /*
     * INMP441 requires 64 SCK clocks per stereo frame, i.e. two 32-bit slots.
     * Keep WS at exactly one slot width for a 50% duty cycle.
     */
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.ws_width = 32;

    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s_audio.tx, &std_cfg),
                      fail, TAG, "Failed to initialize I2S TX");
    log_audio_heap("after I2S TX init");
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s_audio.rx, &std_cfg),
                      fail, TAG, "Failed to initialize I2S RX");
    log_audio_heap("after I2S RX init");

    portENTER_CRITICAL(&s_state_mux);
    s_audio.state = VOICE_AUDIO_STATE_IDLE;
    s_audio.initialized = true;
    s_audio.initializing = false;
    s_audio.transitioning = false;
    s_audio.io_active = false;
    portEXIT_CRITICAL(&s_state_mux);

    ESP_LOGI(TAG,
             "ready rate=%dHz bclk=%d ws=%d mic=%d spk=%d frame=32bit-stereo tx_underrun=zero",
             CONFIG_VOICE_AUDIO_SAMPLE_RATE,
             CONFIG_VOICE_AUDIO_BCLK_GPIO,
             CONFIG_VOICE_AUDIO_WS_GPIO,
             CONFIG_VOICE_AUDIO_MIC_DATA_GPIO,
             CONFIG_VOICE_AUDIO_SPK_DATA_GPIO);
    return ESP_OK;

fail:
    audio_release_resources();
    portENTER_CRITICAL(&s_state_mux);
    s_audio.initialized = false;
    s_audio.initializing = false;
    s_audio.transitioning = false;
    s_audio.io_active = false;
    s_audio.state = VOICE_AUDIO_STATE_UNINITIALIZED;
    portEXIT_CRITICAL(&s_state_mux);
    return ret;
}

esp_err_t voice_audio_deinit(void)
{
    bool already_deinitialized = false;
    bool claimed = false;

    portENTER_CRITICAL(&s_state_mux);
    if (!s_audio.initialized && !s_audio.initializing) {
        already_deinitialized = true;
    } else if (s_audio.initialized && !s_audio.initializing &&
               !s_audio.transitioning && !s_audio.io_active &&
               s_audio.state == VOICE_AUDIO_STATE_IDLE) {
        s_audio.transitioning = true;
        claimed = true;
    }
    portEXIT_CRITICAL(&s_state_mux);

    if (already_deinitialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(claimed, ESP_ERR_INVALID_STATE, TAG, "Audio is busy");

    audio_release_resources();
    portENTER_CRITICAL(&s_state_mux);
    s_audio.initialized = false;
    s_audio.transitioning = false;
    s_audio.io_active = false;
    s_audio.state = VOICE_AUDIO_STATE_UNINITIALIZED;
    portEXIT_CRITICAL(&s_state_mux);
    return ESP_OK;
}

void voice_audio_get_info(voice_audio_info_t *info)
{
    if (!info) {
        return;
    }
    memset(info, 0, sizeof(*info));
    info->sample_rate_hz = CONFIG_VOICE_AUDIO_SAMPLE_RATE;
    info->pcm_bits = 16;
    info->channels = 1;
    info->bclk_gpio = CONFIG_VOICE_AUDIO_BCLK_GPIO;
    info->ws_gpio = CONFIG_VOICE_AUDIO_WS_GPIO;
    info->mic_data_gpio = CONFIG_VOICE_AUDIO_MIC_DATA_GPIO;
    info->spk_data_gpio = CONFIG_VOICE_AUDIO_SPK_DATA_GPIO;
    portENTER_CRITICAL(&s_state_mux);
    info->state = s_audio.state;
    info->initialized = s_audio.initialized;
    portEXIT_CRITICAL(&s_state_mux);
}

/*
 * The TX channel is initialized first, so in the ESP-IDF full-duplex pair it
 * remains the clock master and RX becomes the internal slave. During capture
 * we therefore keep TX running with zero PCM frames. This continuously drives
 * BCLK/WS for the INMP441 without producing audible speaker data.
 */
static esp_err_t capture_clock_and_read_raw(size_t frames,
                                            size_t *out_frames,
                                            uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(frames > 0 && frames <= VOICE_AUDIO_IO_FRAMES && out_frames,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid capture clock/read request");

    *out_frames = 0;
    const size_t total_bytes = frames * VOICE_AUDIO_FRAME_BYTES;

    memset(s_audio.tx_raw, 0, total_bytes);

    size_t sent_bytes = 0;
    while (sent_bytes < total_bytes) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(
            s_audio.tx,
            (const uint8_t *)s_audio.tx_raw + sent_bytes,
            total_bytes - sent_bytes,
            &written,
            timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
        if (written == 0) {
            return ESP_ERR_TIMEOUT;
        }
        sent_bytes += written;
    }

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(
        s_audio.rx,
        s_audio.rx_raw,
        total_bytes,
        &bytes_read,
        timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    size_t got_frames = bytes_read / VOICE_AUDIO_FRAME_BYTES;
    if (got_frames == 0) {
        return ESP_ERR_TIMEOUT;
    }

    *out_frames = got_frames;
    return ESP_OK;
}

static esp_err_t capture_discard_settle_data(void)
{
    uint64_t frames_left =
        ((uint64_t)CONFIG_VOICE_AUDIO_SAMPLE_RATE * CONFIG_VOICE_AUDIO_MIC_SETTLE_MS) / 1000U;

    while (frames_left > 0) {
        size_t frames = frames_left > VOICE_AUDIO_IO_FRAMES ?
                        VOICE_AUDIO_IO_FRAMES : (size_t)frames_left;
        size_t got_frames = 0;
        esp_err_t err = capture_clock_and_read_raw(
            frames, &got_frames, VOICE_AUDIO_DEFAULT_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
        frames_left -= got_frames;
    }
    return ESP_OK;
}

esp_err_t voice_audio_capture_start(void)
{
    ESP_RETURN_ON_ERROR(voice_audio_init(), TAG, "Audio init failed");
    ESP_RETURN_ON_FALSE(
        try_begin_transition(VOICE_AUDIO_STATE_IDLE, VOICE_AUDIO_STATE_CAPTURE),
        ESP_ERR_INVALID_STATE, TAG, "Audio is busy");

    /*
     * Full-duplex clock ownership: TX was initialized first and is the master.
     * RX is the paired internal slave, so both channels must be active while
     * recording. TX carries zeros only; its purpose here is to supply BCLK/WS.
     */
    esp_err_t err = i2s_channel_enable(s_audio.tx);
    if (err != ESP_OK) {
        finish_transition(VOICE_AUDIO_STATE_IDLE);
        return err;
    }
    ESP_LOGI(TAG, "capture clock master enabled (TX)");

    err = i2s_channel_enable(s_audio.rx);
    if (err != ESP_OK) {
        (void)i2s_channel_disable(s_audio.tx);
        finish_transition(VOICE_AUDIO_STATE_IDLE);
        return err;
    }
    ESP_LOGI(TAG, "capture receiver enabled (RX)");

    err = capture_discard_settle_data();
    if (err != ESP_OK) {
        (void)i2s_channel_disable(s_audio.rx);
        (void)i2s_channel_disable(s_audio.tx);
        finish_transition(VOICE_AUDIO_STATE_IDLE);
        ESP_LOGE(TAG, "Microphone settle read failed: %s", esp_err_to_name(err));
        return err;
    }

    finish_transition(VOICE_AUDIO_STATE_CAPTURE);
    ESP_LOGI(TAG, "capture started");
    return ESP_OK;
}

esp_err_t voice_audio_capture_read(int16_t *samples,
                                   size_t capacity_samples,
                                   size_t *out_samples,
                                   uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(samples && out_samples && capacity_samples > 0,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid capture buffer");
    ESP_RETURN_ON_FALSE(try_begin_io(VOICE_AUDIO_STATE_CAPTURE),
                        ESP_ERR_INVALID_STATE, TAG, "Capture is not active or is busy");

    esp_err_t ret = ESP_OK;
    *out_samples = 0;
    while (*out_samples < capacity_samples) {
        size_t remaining = capacity_samples - *out_samples;
        size_t frames = remaining > VOICE_AUDIO_IO_FRAMES ?
                        VOICE_AUDIO_IO_FRAMES : remaining;
        size_t got_frames = 0;

        /* Feed zero TX frames so the full-duplex clock master stays active. */
        esp_err_t err = capture_clock_and_read_raw(frames, &got_frames, timeout_ms);
        if (err != ESP_OK) {
            ret = err;
            break;
        }

        for (size_t i = 0; i < got_frames; i++) {
            /* L/R is tied low, therefore the INMP441 drives the left slot. */
            samples[*out_samples + i] = raw_i2s_to_pcm16(s_audio.rx_raw[i * 2U]);
        }
        *out_samples += got_frames;

        if (got_frames < frames) {
            break;
        }
    }
    finish_io();
    return ret;
}

esp_err_t voice_audio_capture_stop(void)
{
    ESP_RETURN_ON_FALSE(
        try_begin_transition(VOICE_AUDIO_STATE_CAPTURE, VOICE_AUDIO_STATE_CAPTURE),
        ESP_ERR_INVALID_STATE, TAG, "Capture is not active or is busy");

    esp_err_t rx_err = i2s_channel_disable(s_audio.rx);
    esp_err_t tx_err = i2s_channel_disable(s_audio.tx);
    finish_transition(VOICE_AUDIO_STATE_IDLE);

    if (rx_err == ESP_OK && tx_err == ESP_OK) {
        ESP_LOGI(TAG, "capture stopped");
        return ESP_OK;
    }
    return rx_err != ESP_OK ? rx_err : tx_err;
}

static esp_err_t playback_write_silence_ms(uint32_t duration_ms)
{
    const size_t frames =
        ((size_t)CONFIG_VOICE_AUDIO_SAMPLE_RATE * duration_ms) / 1000U;
    size_t left = frames;

    memset(s_audio.tx_raw, 0, VOICE_AUDIO_IO_FRAMES * VOICE_AUDIO_FRAME_BYTES);
    while (left > 0) {
        size_t chunk = left > VOICE_AUDIO_IO_FRAMES ? VOICE_AUDIO_IO_FRAMES : left;
        size_t written = 0;
        esp_err_t err = i2s_channel_write(
            s_audio.tx,
            s_audio.tx_raw,
            chunk * VOICE_AUDIO_FRAME_BYTES,
            &written,
            VOICE_AUDIO_DEFAULT_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
        if (written == 0) {
            return ESP_ERR_TIMEOUT;
        }
        left -= written / VOICE_AUDIO_FRAME_BYTES;
    }
    return ESP_OK;
}

esp_err_t voice_audio_playback_start(void)
{
    ESP_RETURN_ON_ERROR(voice_audio_init(), TAG, "Audio init failed");
    ESP_RETURN_ON_FALSE(
        try_begin_transition(VOICE_AUDIO_STATE_IDLE, VOICE_AUDIO_STATE_PLAYBACK),
        ESP_ERR_INVALID_STATE, TAG, "Audio is busy");
    esp_err_t err = i2s_channel_enable(s_audio.tx);
    if (err != ESP_OK) {
        finish_transition(VOICE_AUDIO_STATE_IDLE);
        return err;
    }

    /* Keep valid clocks running long enough for the Class-D amplifier to wake. */
    err = playback_write_silence_ms(VOICE_AUDIO_START_SILENCE_MS);
    if (err != ESP_OK) {
        (void)i2s_channel_disable(s_audio.tx);
        finish_transition(VOICE_AUDIO_STATE_IDLE);
        return err;
    }

    finish_transition(VOICE_AUDIO_STATE_PLAYBACK);
    ESP_LOGI(TAG, "playback started");
    return ESP_OK;
}

esp_err_t voice_audio_playback_write(const int16_t *samples,
                                     size_t sample_count,
                                     uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(samples && sample_count > 0,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid playback buffer");
    ESP_RETURN_ON_FALSE(try_begin_io(VOICE_AUDIO_STATE_PLAYBACK),
                        ESP_ERR_INVALID_STATE, TAG, "Playback is not active or is busy");

    esp_err_t ret = ESP_OK;
    size_t offset = 0;
    while (offset < sample_count) {
        size_t frames = sample_count - offset;
        if (frames > VOICE_AUDIO_IO_FRAMES) {
            frames = VOICE_AUDIO_IO_FRAMES;
        }

        for (size_t i = 0; i < frames; i++) {
            int32_t packed = pcm16_to_i2s(samples[offset + i]);
            /* Duplicate mono to both channels for MAX98357A channel selection. */
            s_audio.tx_raw[i * 2U] = packed;
            s_audio.tx_raw[i * 2U + 1U] = packed;
        }

        size_t total_bytes = frames * VOICE_AUDIO_FRAME_BYTES;
        size_t sent_bytes = 0;
        while (sent_bytes < total_bytes) {
            size_t written = 0;
            esp_err_t err = i2s_channel_write(
                s_audio.tx,
                (const uint8_t *)s_audio.tx_raw + sent_bytes,
                total_bytes - sent_bytes,
                &written,
                timeout_ms);
            if (err != ESP_OK) {
                ret = err;
                goto done;
            }
            if (written == 0) {
                ret = ESP_ERR_TIMEOUT;
                goto done;
            }
            sent_bytes += written;
        }

        offset += frames;
    }

done:
    finish_io();
    return ret;
}

esp_err_t voice_audio_playback_stop(void)
{
    ESP_RETURN_ON_FALSE(
        try_begin_transition(VOICE_AUDIO_STATE_PLAYBACK, VOICE_AUDIO_STATE_PLAYBACK),
        ESP_ERR_INVALID_STATE, TAG, "Playback is not active or is busy");

    esp_err_t tail_err = playback_write_silence_ms(VOICE_AUDIO_TAIL_SILENCE_MS);
    if (tail_err == ESP_OK) {
        /* i2s_channel_write() returns after handing data to DMA. Keep TX
         * enabled past the three-descriptor ring so the final samples reach
         * the amplifier before i2s_channel_disable() stops BCLK/WS. */
        vTaskDelay(pdMS_TO_TICKS(VOICE_AUDIO_DMA_DRAIN_MS));
    }
    esp_err_t disable_err = i2s_channel_disable(s_audio.tx);
    finish_transition(VOICE_AUDIO_STATE_IDLE);

    if (tail_err != ESP_OK) {
        return tail_err;
    }
    if (disable_err == ESP_OK) {
        ESP_LOGI(TAG, "playback stopped");
    }
    return disable_err;
}

esp_err_t voice_audio_record_wav(const char *path,
                                 uint32_t duration_ms,
                                 voice_audio_stats_t *out_stats)
{
    ESP_RETURN_ON_FALSE(path && path[0] && duration_ms > 0,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid WAV record arguments");

    esp_err_t ret = ESP_OK;
    FILE *fp = NULL;
    int16_t *pcm = NULL;
    bool capture_started = false;
    uint64_t sum_sq = 0;
    uint32_t samples_written = 0;
    int32_t peak = 0;

    if (out_stats) {
        memset(out_stats, 0, sizeof(*out_stats));
    }

    fp = fopen(path, "wb+");
    ESP_GOTO_ON_FALSE(fp, ESP_FAIL, cleanup, TAG,
                      "Open record file failed: %s (%s)", path, strerror(errno));

    /* Reserve a valid header first. It is rewritten with final sizes on success. */
    ESP_GOTO_ON_ERROR(wav_write_header(fp, 0), cleanup, TAG, "Initial WAV header failed");

    pcm = heap_caps_malloc(VOICE_AUDIO_IO_FRAMES * sizeof(int16_t),
                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_GOTO_ON_FALSE(pcm, ESP_ERR_NO_MEM, cleanup, TAG, "PCM buffer allocation failed");

    ESP_GOTO_ON_ERROR(voice_audio_capture_start(), cleanup, TAG, "Capture start failed");
    capture_started = true;

    const uint64_t target_samples =
        ((uint64_t)CONFIG_VOICE_AUDIO_SAMPLE_RATE * duration_ms) / 1000U;

    while ((uint64_t)samples_written < target_samples) {
        size_t want = (size_t)(target_samples - samples_written);
        if (want > VOICE_AUDIO_IO_FRAMES) {
            want = VOICE_AUDIO_IO_FRAMES;
        }

        size_t got = 0;
        ESP_GOTO_ON_ERROR(
            voice_audio_capture_read(pcm, want, &got, VOICE_AUDIO_DEFAULT_TIMEOUT_MS),
            cleanup, TAG, "Capture read failed");
        ESP_GOTO_ON_FALSE(got > 0, ESP_ERR_TIMEOUT, cleanup, TAG, "Capture returned no data");

        ESP_GOTO_ON_FALSE(fwrite(pcm, sizeof(int16_t), got, fp) == got,
                          ESP_FAIL, cleanup, TAG, "WAV data write failed: %s", strerror(errno));

        for (size_t i = 0; i < got; i++) {
            int32_t sample = pcm[i];
            int32_t mag = sample < 0 ? -sample : sample;
            if (mag > peak) {
                peak = mag;
            }
            sum_sq += (uint64_t)((int64_t)sample * sample);
        }
        samples_written += (uint32_t)got;
    }

    ESP_GOTO_ON_ERROR(voice_audio_capture_stop(), cleanup, TAG, "Capture stop failed");
    capture_started = false;

    ESP_GOTO_ON_ERROR(wav_write_header(fp, samples_written * VOICE_AUDIO_PCM_BYTES),
                      cleanup, TAG, "Final WAV header failed");
    ESP_GOTO_ON_FALSE(fflush(fp) == 0, ESP_FAIL, cleanup, TAG,
                      "WAV flush failed: %s", strerror(errno));

    if (out_stats) {
        out_stats->samples = samples_written;
        out_stats->pcm_bytes = samples_written * VOICE_AUDIO_PCM_BYTES;
        out_stats->peak = (int16_t)(peak > INT16_MAX ? INT16_MAX : peak);
        if (samples_written > 0) {
            double mean_sq = (double)sum_sq / (double)samples_written;
            double rms = sqrt(mean_sq);
            if (rms > 65535.0) {
                rms = 65535.0;
            }
            out_stats->rms = (uint16_t)rms;
        }
    }

    ESP_LOGI(TAG, "recorded path=%s samples=%u bytes=%u peak=%ld",
             path, (unsigned)samples_written,
             (unsigned)(samples_written * VOICE_AUDIO_PCM_BYTES),
             (long)peak);

cleanup:
    if (capture_started) {
        (void)voice_audio_capture_stop();
    }
    free(pcm);
    if (fp) {
        fclose(fp);
    }
    return ret;
}

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t format;
    long data_offset;
    uint32_t data_size;
} wav_info_t;

static esp_err_t wav_parse(FILE *fp, wav_info_t *info)
{
    uint8_t head[12];
    bool have_fmt = false;
    bool have_data = false;

    ESP_RETURN_ON_FALSE(fp && info, ESP_ERR_INVALID_ARG, TAG, "Invalid WAV parser args");
    memset(info, 0, sizeof(*info));

    ESP_RETURN_ON_FALSE(fread(head, 1, sizeof(head), fp) == sizeof(head),
                        ESP_ERR_INVALID_SIZE, TAG, "WAV header too short");
    ESP_RETURN_ON_FALSE(memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WAVE", 4) == 0,
                        ESP_ERR_INVALID_RESPONSE, TAG, "Not a RIFF/WAVE file");

    while (!have_data) {
        uint8_t chunk_head[8];
        if (fread(chunk_head, 1, sizeof(chunk_head), fp) != sizeof(chunk_head)) {
            return ESP_ERR_INVALID_SIZE;
        }

        uint32_t chunk_size = get_le32(chunk_head + 4);
        long payload_pos = ftell(fp);
        ESP_RETURN_ON_FALSE(payload_pos >= 0, ESP_FAIL, TAG, "ftell failed");

        if (memcmp(chunk_head, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            ESP_RETURN_ON_FALSE(chunk_size >= sizeof(fmt), ESP_ERR_INVALID_SIZE, TAG,
                                "WAV fmt chunk too short");
            ESP_RETURN_ON_FALSE(fread(fmt, 1, sizeof(fmt), fp) == sizeof(fmt),
                                ESP_ERR_INVALID_SIZE, TAG, "WAV fmt read failed");

            info->format = get_le16(fmt + 0);
            info->channels = get_le16(fmt + 2);
            info->sample_rate = get_le32(fmt + 4);
            info->bits_per_sample = get_le16(fmt + 14);
            have_fmt = true;
        } else if (memcmp(chunk_head, "data", 4) == 0) {
            info->data_offset = payload_pos;
            info->data_size = chunk_size;
            have_data = true;
            break;
        }

        long next = payload_pos + (long)chunk_size + (long)(chunk_size & 1U);
        ESP_RETURN_ON_FALSE(fseek(fp, next, SEEK_SET) == 0, ESP_FAIL, TAG,
                            "WAV chunk seek failed");
    }

    ESP_RETURN_ON_FALSE(have_fmt && have_data, ESP_ERR_INVALID_RESPONSE, TAG,
                        "Missing WAV fmt/data chunk");
    ESP_RETURN_ON_FALSE(info->format == 1, ESP_ERR_NOT_SUPPORTED, TAG,
                        "Only PCM WAV is supported");
    ESP_RETURN_ON_FALSE(info->channels == 1, ESP_ERR_NOT_SUPPORTED, TAG,
                        "Only mono WAV is supported");
    ESP_RETURN_ON_FALSE(info->bits_per_sample == 16, ESP_ERR_NOT_SUPPORTED, TAG,
                        "Only 16-bit WAV is supported");
    ESP_RETURN_ON_FALSE(info->sample_rate == CONFIG_VOICE_AUDIO_SAMPLE_RATE,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "WAV sample rate %u does not match configured %u",
                        (unsigned)info->sample_rate,
                        (unsigned)CONFIG_VOICE_AUDIO_SAMPLE_RATE);
    ESP_RETURN_ON_FALSE((info->data_size % sizeof(int16_t)) == 0,
                        ESP_ERR_INVALID_SIZE, TAG, "WAV PCM data size is odd");

    return ESP_OK;
}

esp_err_t voice_audio_play_wav(const char *path)
{
    ESP_RETURN_ON_FALSE(path && path[0], ESP_ERR_INVALID_ARG, TAG, "Invalid WAV path");

    esp_err_t ret = ESP_OK;
    FILE *fp = NULL;
    int16_t *pcm = NULL;
    bool playback_started = false;
    wav_info_t info = {0};

    fp = fopen(path, "rb");
    ESP_GOTO_ON_FALSE(fp, ESP_FAIL, cleanup, TAG,
                      "Open playback file failed: %s (%s)", path, strerror(errno));
    ESP_GOTO_ON_ERROR(wav_parse(fp, &info), cleanup, TAG, "WAV parse failed");
    ESP_GOTO_ON_FALSE(fseek(fp, info.data_offset, SEEK_SET) == 0,
                      ESP_FAIL, cleanup, TAG, "Seek to PCM data failed");

    pcm = heap_caps_malloc(VOICE_AUDIO_IO_FRAMES * sizeof(int16_t),
                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_GOTO_ON_FALSE(pcm, ESP_ERR_NO_MEM, cleanup, TAG, "PCM buffer allocation failed");

    ESP_GOTO_ON_ERROR(voice_audio_playback_start(), cleanup, TAG, "Playback start failed");
    playback_started = true;

    uint32_t bytes_left = info.data_size;
    while (bytes_left > 0) {
        size_t samples_wanted = bytes_left / sizeof(int16_t);
        if (samples_wanted > VOICE_AUDIO_IO_FRAMES) {
            samples_wanted = VOICE_AUDIO_IO_FRAMES;
        }

        size_t got = fread(pcm, sizeof(int16_t), samples_wanted, fp);
        ESP_GOTO_ON_FALSE(got > 0, ESP_FAIL, cleanup, TAG,
                          "Unexpected EOF while reading WAV PCM");

        ESP_GOTO_ON_ERROR(
            voice_audio_playback_write(pcm, got, VOICE_AUDIO_DEFAULT_TIMEOUT_MS),
            cleanup, TAG, "Playback write failed");
        bytes_left -= (uint32_t)(got * sizeof(int16_t));
    }

    ESP_GOTO_ON_ERROR(voice_audio_playback_stop(), cleanup, TAG, "Playback stop failed");
    playback_started = false;

    ESP_LOGI(TAG, "played path=%s bytes=%u", path, (unsigned)info.data_size);

cleanup:
    if (playback_started) {
        (void)voice_audio_playback_stop();
    }
    free(pcm);
    if (fp) {
        fclose(fp);
    }
    return ret;
}
