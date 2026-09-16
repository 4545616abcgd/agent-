/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lua binding for ESP-IDF I2S RX (digital microphone input).
 * Supports INMP441 and similar I2S MEMS microphones.
 * Usage: local i2s = require("i2s")
 */
#include "lua_driver_i2s.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lauxlib.h"

#define LUA_DRIVER_I2S_METATABLE "i2s.handle"
#define LUA_DRIVER_I2S_TAG       "lua_driver_i2s"
#define LUA_DRIVER_I2S_CHUNK_MAX (128 * 1024) /* 128 KB max per read */

/* ------------------------------------------------------------------ */
/*  userdata                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    i2s_chan_handle_t rx_handle;
    i2s_port_t port;
    bool installed;
    int bclk_pin;
    int lrck_pin;
    int data_in_pin;
    int sample_rate;
    int bits_per_sample;
} lua_driver_i2s_ud_t;

/* ------------------------------------------------------------------ */
/*  internal helpers                                                   */
/* ------------------------------------------------------------------ */

static lua_driver_i2s_ud_t *i2s_get_ud(lua_State *L, int idx)
{
    lua_driver_i2s_ud_t *ud = (lua_driver_i2s_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_I2S_METATABLE);
    if (!ud->installed) {
        luaL_error(L, "i2s handle is closed");
    }
    return ud;
}

static int i2s_chan_to_id(i2s_port_t port)
{
    switch (port) {
    case I2S_NUM_0:
        return 0;
#if SOC_I2S_NUM > 1
    case I2S_NUM_1:
        return 1;
#endif
    default:
        return -1;
    }
}

static i2s_port_t i2s_id_to_chan(int id)
{
    switch (id) {
    case 0:
        return I2S_NUM_0;
#if SOC_I2S_NUM > 1
    case 1:
        return I2S_NUM_1;
#endif
    default:
        return I2S_NUM_0;
    }
}

/* ------------------------------------------------------------------ */
/*  channel format helper                                              */
/* ------------------------------------------------------------------ */

static i2s_data_bit_width_t lua_to_i2s_bits(int bits)
{
    switch (bits) {
    case 16:
        return I2S_DATA_BIT_WIDTH_16BIT;
    case 24:
        return I2S_DATA_BIT_WIDTH_24BIT;
    case 32:
        return I2S_DATA_BIT_WIDTH_32BIT;
    default:
        return I2S_DATA_BIT_WIDTH_24BIT;
    }
}

/* ------------------------------------------------------------------ */
/*  handle:close()                                                     */
/* ------------------------------------------------------------------ */

static int i2s_handle_close(lua_State *L)
{
    lua_driver_i2s_ud_t *ud = (lua_driver_i2s_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_I2S_METATABLE);

    if (ud->installed) {
        esp_err_t err = i2s_channel_disable(ud->rx_handle);
        if (err != ESP_OK) {
            ESP_LOGW(LUA_DRIVER_I2S_TAG,
                     "i2s_channel_disable failed: %s", esp_err_to_name(err));
        }
        err = i2s_del_channel(ud->rx_handle);
        if (err != ESP_OK) {
            ESP_LOGW(LUA_DRIVER_I2S_TAG,
                     "i2s_del_channel failed: %s", esp_err_to_name(err));
        }
        ud->installed = false;
        ud->rx_handle = NULL;

        ESP_LOGI(LUA_DRIVER_I2S_TAG,
                 "I2S port %d closed", i2s_chan_to_id(ud->port));
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  __gc                                                               */
/* ------------------------------------------------------------------ */

static int i2s_handle_gc(lua_State *L)
{
    lua_driver_i2s_ud_t *ud = (lua_driver_i2s_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_I2S_METATABLE);
    if (ud && ud->installed) {
        i2s_channel_disable(ud->rx_handle);
        i2s_del_channel(ud->rx_handle);
        ud->installed = false;
        ud->rx_handle = NULL;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  __tostring                                                         */
/* ------------------------------------------------------------------ */

static int i2s_handle_tostring(lua_State *L)
{
    lua_driver_i2s_ud_t *ud = (lua_driver_i2s_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_I2S_METATABLE);
    lua_pushfstring(L, "i2s handle (port=%d, %d Hz %dbit)",
                    i2s_chan_to_id(ud->port),
                    ud->sample_rate,
                    ud->bits_per_sample);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  handle:read() — single sample (4 bytes / int32_t)                   */
/* ------------------------------------------------------------------ */

static int i2s_handle_read(lua_State *L)
{
    lua_driver_i2s_ud_t *ud = i2s_get_ud(L, 1);
    int32_t sample = 0;
    size_t bytes_read = 0;

    esp_err_t err = i2s_channel_read(ud->rx_handle, &sample, sizeof(sample),
                                     &bytes_read, portMAX_DELAY);
    if (err != ESP_OK) {
        return luaL_error(L, "i2s read failed: %s", esp_err_to_name(err));
    }
    if (bytes_read < sizeof(sample)) {
        lua_pushinteger(L, 0);
        return 1;
    }

    /* 24-bit sign extension: shift left 8, then arithmetic shift right 8 */
    if (ud->bits_per_sample == 24) {
        sample = (sample << 8) >> 8;
    }

    lua_pushinteger(L, (lua_Integer)sample);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  handle:read_n(count) — batch read into table of int32_t             */
/* ------------------------------------------------------------------ */

static int i2s_handle_read_n(lua_State *L)
{
    lua_driver_i2s_ud_t *ud = i2s_get_ud(L, 1);
    lua_Integer count = luaL_checkinteger(L, 2);

    if (count <= 0) {
        return luaL_error(L, "i2s read_n count must be positive");
    }

    size_t total_bytes = (size_t)count * sizeof(int32_t);
    if (total_bytes > LUA_DRIVER_I2S_CHUNK_MAX) {
        return luaL_error(L, "i2s read_n total size exceeds %d bytes",
                          LUA_DRIVER_I2S_CHUNK_MAX);
    }

    int32_t *buf = (int32_t *)malloc(total_bytes);
    if (!buf) {
        return luaL_error(L, "i2s read_n out of memory");
    }

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(ud->rx_handle, buf, total_bytes,
                                     &bytes_read, portMAX_DELAY);
    if (err != ESP_OK) {
        free(buf);
        return luaL_error(L, "i2s read_n failed: %s", esp_err_to_name(err));
    }

    lua_Integer valid_samples = (lua_Integer)(bytes_read / sizeof(int32_t));
    lua_createtable(L, (int)valid_samples, 0);

    for (lua_Integer i = 0; i < valid_samples; i++) {
        int32_t s = buf[i];
        if (ud->bits_per_sample == 24) {
            s = (s << 8) >> 8;
        }
        lua_pushinteger(L, (lua_Integer)s);
        lua_rawseti(L, -2, i + 1);
    }

    free(buf);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  handle:read_raw(count) — batch read into Lua binary string          */
/* ------------------------------------------------------------------ */

static int i2s_handle_read_raw(lua_State *L)
{
    lua_driver_i2s_ud_t *ud = i2s_get_ud(L, 1);
    lua_Integer count = luaL_checkinteger(L, 2);

    if (count <= 0) {
        return luaL_error(L, "i2s read_raw count must be positive");
    }

    size_t total_bytes = (size_t)count * sizeof(int32_t);
    if (total_bytes > LUA_DRIVER_I2S_CHUNK_MAX) {
        return luaL_error(L, "i2s read_raw total size exceeds %d bytes",
                          LUA_DRIVER_I2S_CHUNK_MAX);
    }

    luaL_Buffer b;
    int32_t *buf = (int32_t *)luaL_buffinitsize(L, &b, total_bytes);

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(ud->rx_handle, buf, total_bytes,
                                     &bytes_read, portMAX_DELAY);
    if (err != ESP_OK) {
        return luaL_error(L, "i2s read_raw failed: %s", esp_err_to_name(err));
    }

    luaL_pushresultsize(&b, bytes_read);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  i2s.calc_rms(samples) — static method                               */
/*  samples: table of integers, or raw binary string (int32_t LE)       */
/* ------------------------------------------------------------------ */

static int i2s_calc_rms(lua_State *L)
{
    int type = lua_type(L, 1);
    double sum_sq = 0.0;
    int64_t count = 0;

    if (type == LUA_TTABLE) {
        lua_Integer n = luaL_len(L, 1);
        for (lua_Integer i = 1; i <= n; i++) {
            lua_rawgeti(L, 1, i);
            lua_Integer s = luaL_checkinteger(L, -1);
            sum_sq += (double)s * (double)s;
            count++;
            lua_pop(L, 1);
        }
    } else if (type == LUA_TSTRING) {
        size_t len = 0;
        const char *data = lua_tolstring(L, 1, &len);
        count = (int64_t)(len / sizeof(int32_t));
        const int32_t *samples = (const int32_t *)data;
        for (int64_t i = 0; i < count; i++) {
            int32_t s = samples[i];
            sum_sq += (double)s * (double)s;
        }
    } else {
        return luaL_error(L, "i2s calc_rms expects a table or string");
    }

    if (count == 0) {
        lua_pushnumber(L, 0.0);
        return 1;
    }

    double rms = sqrt(sum_sq / (double)count);
    lua_pushnumber(L, rms);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  i2s.calc_db(samples) — static method                                */
/*  24-bit full-scale = 2^23 = 8388608                                  */
/* ------------------------------------------------------------------ */

static int i2s_calc_db(lua_State *L)
{
    int type = lua_type(L, 1);
    double sum_sq = 0.0;
    int64_t count = 0;

    if (type == LUA_TTABLE) {
        lua_Integer n = luaL_len(L, 1);
        for (lua_Integer i = 1; i <= n; i++) {
            lua_rawgeti(L, 1, i);
            lua_Integer s = luaL_checkinteger(L, -1);
            sum_sq += (double)s * (double)s;
            count++;
            lua_pop(L, 1);
        }
    } else if (type == LUA_TSTRING) {
        size_t len = 0;
        const char *data = lua_tolstring(L, 1, &len);
        count = (int64_t)(len / sizeof(int32_t));
        const int32_t *samples = (const int32_t *)data;
        for (int64_t i = 0; i < count; i++) {
            int32_t s = samples[i];
            sum_sq += (double)s * (double)s;
        }
    } else {
        return luaL_error(L, "i2s calc_db expects a table or string");
    }

    if (count == 0) {
        lua_pushnumber(L, -96.3);
        return 1;
    }

    double rms = sqrt(sum_sq / (double)count);
    /* 24-bit full-scale peak = 2^23 */
    double full_scale = 8388608.0;
    double ratio = rms / full_scale;
    double db;

    if (ratio <= 0.0) {
        db = -96.3;
    } else {
        db = 20.0 * log10(ratio);
    }

    lua_pushnumber(L, db);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  i2s.new(config) — create I2S RX handle                              */
/* ------------------------------------------------------------------ */

static int i2s_new(lua_State *L)
{
    /* Validate arg is a table */
    luaL_checktype(L, 1, LUA_TTABLE);

    /* Read config fields */
    lua_getfield(L, 1, "bclk_pin");
    int bclk = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "lrck_pin");
    int lrck = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "data_in_pin");
    int data_in = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "sample_rate");
    int sample_rate = (int)luaL_optinteger(L, -1, 16000);
    lua_pop(L, 1);

    lua_getfield(L, 1, "bits_per_sample");
    int bits = (int)luaL_optinteger(L, -1, 24);
    lua_pop(L, 1);

    lua_getfield(L, 1, "port");
    int port_id = (int)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);

    lua_getfield(L, 1, "dma_buf_count");
    int dma_buf_count = (int)luaL_optinteger(L, -1, 8);
    lua_pop(L, 1);

    lua_getfield(L, 1, "dma_buf_len");
    int dma_buf_len = (int)luaL_optinteger(L, -1, 64);
    lua_pop(L, 1);

    lua_getfield(L, 1, "channel");
    const char *chan_str = luaL_optstring(L, -1, "left");
    lua_pop(L, 1);

    i2s_slot_mode_t chan_mode = I2S_SLOT_MODE_MONO;
    if (strcmp(chan_str, "right") == 0) {
        chan_mode = I2S_SLOT_MODE_MONO;  /* mono right via slot config */
    }

    if (bclk < 0 || lrck < 0 || data_in < 0) {
        return luaL_error(L, "i2s: bclk_pin, lrck_pin, data_in_pin are required");
    }

    i2s_port_t port = i2s_id_to_chan(port_id);
    i2s_data_bit_width_t bit_width = lua_to_i2s_bits(bits);

    /* --- std mode config --- */
    i2s_chan_config_t chan_cfg = {
        .id = port,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = dma_buf_count,
        .dma_frame_num = dma_buf_len,
        .auto_clear = true,
    };

    i2s_chan_handle_t rx_handle = NULL;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (err != ESP_OK) {
        return luaL_error(L, "i2s: new_channel failed on port %d: %s",
                          port_id, esp_err_to_name(err));
    }

    /* --- std config for RX --- */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width, chan_mode),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk,
            .ws = lrck,
            .dout = I2S_GPIO_UNUSED,
            .din = data_in,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(rx_handle);
        return luaL_error(L, "i2s: init_std_mode failed: %s",
                          esp_err_to_name(err));
    }

    err = i2s_channel_enable(rx_handle);
    if (err != ESP_OK) {
        i2s_del_channel(rx_handle);
        return luaL_error(L, "i2s: channel_enable failed: %s",
                          esp_err_to_name(err));
    }

    ESP_LOGI(LUA_DRIVER_I2S_TAG,
             "I2S initialized: port=%d bclk=%d lrck=%d din=%d rate=%d bits=%d",
             port_id, bclk, lrck, data_in, sample_rate, bits);

    /* Create userdata */
    lua_driver_i2s_ud_t *ud = (lua_driver_i2s_ud_t *)lua_newuserdata(
        L, sizeof(lua_driver_i2s_ud_t));
    ud->rx_handle = rx_handle;
    ud->port = port;
    ud->installed = true;
    ud->bclk_pin = bclk;
    ud->lrck_pin = lrck;
    ud->data_in_pin = data_in;
    ud->sample_rate = sample_rate;
    ud->bits_per_sample = bits;

    luaL_getmetatable(L, LUA_DRIVER_I2S_METATABLE);
    lua_setmetatable(L, -2);

    return 1;
}

/* ------------------------------------------------------------------ */
/*  module registration                                                */
/* ------------------------------------------------------------------ */

int luaopen_i2s(lua_State *L)
{
    /* Handle metatable */
    if (luaL_newmetatable(L, LUA_DRIVER_I2S_METATABLE)) {
        /* __gc */
        lua_pushcfunction(L, i2s_handle_gc);
        lua_setfield(L, -2, "__gc");

        /* __tostring */
        lua_pushcfunction(L, i2s_handle_tostring);
        lua_setfield(L, -2, "__tostring");

        /* __index = metatable (self-indexing) */
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");

        /* Methods */
        lua_pushcfunction(L, i2s_handle_close);
        lua_setfield(L, -2, "close");

        lua_pushcfunction(L, i2s_handle_read);
        lua_setfield(L, -2, "read");

        lua_pushcfunction(L, i2s_handle_read_n);
        lua_setfield(L, -2, "read_n");

        lua_pushcfunction(L, i2s_handle_read_raw);
        lua_setfield(L, -2, "read_raw");
    }
    lua_pop(L, 1);

    /* Module table */
    lua_newtable(L);

    lua_pushcfunction(L, i2s_new);
    lua_setfield(L, -2, "new");

    lua_pushcfunction(L, i2s_calc_rms);
    lua_setfield(L, -2, "calc_rms");

    lua_pushcfunction(L, i2s_calc_db);
    lua_setfield(L, -2, "calc_db");

    return 1;
}

esp_err_t lua_driver_i2s_register(void)
{
    return cap_lua_register_module("i2s", luaopen_i2s);
}
