/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lua binding for ESP-IDF I2S TX (audio output).
 * Supports MAX98357A and similar I2S DAC/amplifiers.
 * Usage: local i2s_tx = require("i2s_tx")
 */
#include "lua_driver_i2s_tx.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lauxlib.h"

#define LUA_DRIVER_I2S_TX_METATABLE "i2s_tx.handle"
#define LUA_DRIVER_I2S_TX_TAG       "lua_driver_i2s_tx"
#define LUA_DRIVER_I2S_TX_CHUNK_MAX (64 * 1024) /* 64 KB max per write */

/* ------------------------------------------------------------------ */
/*  userdata                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    i2s_chan_handle_t tx_handle;
    i2s_port_t port;
    bool installed;
    int bclk_pin;
    int lrck_pin;
    int data_out_pin;
    int sample_rate;
    int bits_per_sample;
    bool is_stereo;
} lua_driver_i2s_tx_ud_t;

/* ------------------------------------------------------------------ */
/*  internal helpers                                                   */
/* ------------------------------------------------------------------ */

static lua_driver_i2s_tx_ud_t *i2s_tx_get_ud(lua_State *L, int idx)
{
    lua_driver_i2s_tx_ud_t *ud = (lua_driver_i2s_tx_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_I2S_TX_METATABLE);
    if (!ud->installed) {
        luaL_error(L, "i2s_tx handle is closed");
    }
    return ud;
}

static int i2s_tx_chan_to_id(i2s_port_t port)
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

static i2s_port_t i2s_tx_id_to_chan(int id)
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

static i2s_data_bit_width_t lua_to_i2s_tx_bits(int bits)
{
    switch (bits) {
    case 16:
        return I2S_DATA_BIT_WIDTH_16BIT;
    case 24:
        return I2S_DATA_BIT_WIDTH_24BIT;
    case 32:
        return I2S_DATA_BIT_WIDTH_32BIT;
    default:
        return I2S_DATA_BIT_WIDTH_16BIT;
    }
}

/* ------------------------------------------------------------------ */
/*  handle:close()                                                     */
/* ------------------------------------------------------------------ */

static int i2s_tx_handle_close(lua_State *L)
{
    lua_driver_i2s_tx_ud_t *ud = (lua_driver_i2s_tx_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_I2S_TX_METATABLE);

    if (ud->installed) {
        esp_err_t err = i2s_channel_disable(ud->tx_handle);
        if (err != ESP_OK) {
            ESP_LOGW(LUA_DRIVER_I2S_TX_TAG,
                     "i2s_channel_disable failed: %s", esp_err_to_name(err));
        }
        err = i2s_del_channel(ud->tx_handle);
        if (err != ESP_OK) {
            ESP_LOGW(LUA_DRIVER_I2S_TX_TAG,
                     "i2s_del_channel failed: %s", esp_err_to_name(err));
        }
        ud->installed = false;
        ud->tx_handle = NULL;

        ESP_LOGI(LUA_DRIVER_I2S_TX_TAG,
                 "I2S TX port %d closed", i2s_tx_chan_to_id(ud->port));
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  __gc                                                               */
/* ------------------------------------------------------------------ */

static int i2s_tx_handle_gc(lua_State *L)
{
    lua_driver_i2s_tx_ud_t *ud = (lua_driver_i2s_tx_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_I2S_TX_METATABLE);
    if (ud && ud->installed) {
        i2s_channel_disable(ud->tx_handle);
        i2s_del_channel(ud->tx_handle);
        ud->installed = false;
        ud->tx_handle = NULL;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  __tostring                                                         */
/* ------------------------------------------------------------------ */

static int i2s_tx_handle_tostring(lua_State *L)
{
    lua_driver_i2s_tx_ud_t *ud = (lua_driver_i2s_tx_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_I2S_TX_METATABLE);
    lua_pushfstring(L, "i2s_tx handle (port=%d, %d Hz %dbit %s)",
                    i2s_tx_chan_to_id(ud->port),
                    ud->sample_rate,
                    ud->bits_per_sample,
                    ud->is_stereo ? "stereo" : "mono");
    return 1;
}

/* ------------------------------------------------------------------ */
/*  handle:write(data) — write raw PCM bytes to I2S DAC                */
/*  data can be a string (raw PCM bytes) or a table of int16 samples   */
/* ------------------------------------------------------------------ */

static int i2s_tx_handle_write(lua_State *L)
{
    lua_driver_i2s_tx_ud_t *ud = i2s_tx_get_ud(L, 1);
    int type = lua_type(L, 2);

    if (type == LUA_TSTRING) {
        /* Raw PCM bytes */
        size_t len = 0;
        const char *data = lua_tolstring(L, 2, &len);

        if (len == 0) {
            lua_pushboolean(L, 1);
            return 1;
        }

        if (len > LUA_DRIVER_I2S_TX_CHUNK_MAX) {
            return luaL_error(L, "i2s_tx write data exceeds %d bytes",
                              LUA_DRIVER_I2S_TX_CHUNK_MAX);
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(ud->tx_handle, data, len,
                                          &bytes_written, portMAX_DELAY);
        if (err != ESP_OK) {
            return luaL_error(L, "i2s_tx write failed: %s",
                              esp_err_to_name(err));
        }

        lua_pushinteger(L, (lua_Integer)bytes_written);
        return 1;

    } else if (type == LUA_TTABLE) {
        /* Table of int16 samples — pack into buffer */
        lua_Integer n = luaL_len(L, 2);
        if (n <= 0) {
            lua_pushboolean(L, 1);
            return 1;
        }

        size_t total_bytes = (size_t)n * sizeof(int16_t);
        if (total_bytes > LUA_DRIVER_I2S_TX_CHUNK_MAX) {
            return luaL_error(L, "i2s_tx write table exceeds %d bytes",
                              LUA_DRIVER_I2S_TX_CHUNK_MAX);
        }

        int16_t *buf = (int16_t *)malloc(total_bytes);
        if (!buf) {
            return luaL_error(L, "i2s_tx write out of memory");
        }

        for (lua_Integer i = 0; i < n; i++) {
            lua_rawgeti(L, 2, (int)(i + 1));
            lua_Integer s = luaL_checkinteger(L, -1);
            buf[i] = (int16_t)(s & 0xFFFF);
            lua_pop(L, 1);
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(ud->tx_handle, buf, total_bytes,
                                          &bytes_written, portMAX_DELAY);
        free(buf);

        if (err != ESP_OK) {
            return luaL_error(L, "i2s_tx write failed: %s",
                              esp_err_to_name(err));
        }

        lua_pushinteger(L, (lua_Integer)(bytes_written / sizeof(int16_t)));
        return 1;
    }

    return luaL_error(L, "i2s_tx write expects a string (raw PCM) or table of int16");
}

/* ------------------------------------------------------------------ */
/*  handle:stop() — clear output (mute)                                */
/* ------------------------------------------------------------------ */

static int i2s_tx_handle_stop(lua_State *L)
{
    lua_driver_i2s_tx_ud_t *ud = i2s_tx_get_ud(L, 1);

    esp_err_t err = i2s_channel_disable(ud->tx_handle);
    if (err != ESP_OK) {
        return luaL_error(L, "i2s_tx stop failed: %s", esp_err_to_name(err));
    }

    /* Re-enable so it's ready for next write */
    err = i2s_channel_enable(ud->tx_handle);
    if (err != ESP_OK) {
        return luaL_error(L, "i2s_tx re-enable failed: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  i2s_tx.new(config) — create I2S TX handle for MAX98357A            */
/* ------------------------------------------------------------------ */

static int i2s_tx_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "bclk_pin");
    int bclk = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "lrck_pin");
    int lrck = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "data_out_pin");
    int data_out = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "sample_rate");
    int sample_rate = (int)luaL_optinteger(L, -1, 44100);
    lua_pop(L, 1);

    lua_getfield(L, 1, "bits_per_sample");
    int bits = (int)luaL_optinteger(L, -1, 16);
    lua_pop(L, 1);

    lua_getfield(L, 1, "port");
    int port_id = (int)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);

    lua_getfield(L, 1, "dma_buf_count");
    int dma_buf_count = (int)luaL_optinteger(L, -1, 8);
    lua_pop(L, 1);

    lua_getfield(L, 1, "dma_buf_len");
    int dma_buf_len = (int)luaL_optinteger(L, -1, 256);
    lua_pop(L, 1);

    lua_getfield(L, 1, "channel");
    const char *chan_str = luaL_optstring(L, -1, "left");
    lua_pop(L, 1);

    i2s_slot_mode_t chan_mode = I2S_SLOT_MODE_MONO;
    bool is_stereo = false;
    if (strcmp(chan_str, "stereo") == 0) {
        chan_mode = I2S_SLOT_MODE_STEREO;
        is_stereo = true;
    } else if (strcmp(chan_str, "right") == 0) {
        chan_mode = I2S_SLOT_MODE_MONO; /* mono right via slot mask */
    }

    if (bclk < 0 || lrck < 0 || data_out < 0) {
        return luaL_error(L, "i2s_tx: bclk_pin, lrck_pin, data_out_pin are required");
    }

    i2s_port_t port = i2s_tx_id_to_chan(port_id);
    i2s_data_bit_width_t bit_width = lua_to_i2s_tx_bits(bits);

    /* --- std mode config --- */
    i2s_chan_config_t chan_cfg = {
        .id = port,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = dma_buf_count,
        .dma_frame_num = dma_buf_len,
        .auto_clear = true,
    };

    i2s_chan_handle_t tx_handle = NULL;
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (err != ESP_OK) {
        return luaL_error(L, "i2s_tx: new_channel failed on port %d: %s",
                          port_id, esp_err_to_name(err));
    }

    /* --- std config for TX --- */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width, chan_mode),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk,
            .ws = lrck,
            .dout = data_out,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(tx_handle);
        return luaL_error(L, "i2s_tx: init_std_mode failed: %s",
                          esp_err_to_name(err));
    }

    err = i2s_channel_enable(tx_handle);
    if (err != ESP_OK) {
        i2s_del_channel(tx_handle);
        return luaL_error(L, "i2s_tx: channel_enable failed: %s",
                          esp_err_to_name(err));
    }

    ESP_LOGI(LUA_DRIVER_I2S_TX_TAG,
             "I2S TX initialized: port=%d bclk=%d lrck=%d dout=%d rate=%d bits=%d %s",
             port_id, bclk, lrck, data_out, sample_rate, bits,
             is_stereo ? "stereo" : "mono");

    /* Create userdata */
    lua_driver_i2s_tx_ud_t *ud = (lua_driver_i2s_tx_ud_t *)lua_newuserdata(
        L, sizeof(lua_driver_i2s_tx_ud_t));
    ud->tx_handle = tx_handle;
    ud->port = port;
    ud->installed = true;
    ud->bclk_pin = bclk;
    ud->lrck_pin = lrck;
    ud->data_out_pin = data_out;
    ud->sample_rate = sample_rate;
    ud->bits_per_sample = bits;
    ud->is_stereo = is_stereo;

    luaL_getmetatable(L, LUA_DRIVER_I2S_TX_METATABLE);
    lua_setmetatable(L, -2);

    return 1;
}

/* ------------------------------------------------------------------ */
/*  module registration                                                */
/* ------------------------------------------------------------------ */

int luaopen_i2s_tx(lua_State *L)
{
    /* Handle metatable */
    if (luaL_newmetatable(L, LUA_DRIVER_I2S_TX_METATABLE)) {
        lua_pushcfunction(L, i2s_tx_handle_gc);
        lua_setfield(L, -2, "__gc");

        lua_pushcfunction(L, i2s_tx_handle_tostring);
        lua_setfield(L, -2, "__tostring");

        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, i2s_tx_handle_close);
        lua_setfield(L, -2, "close");

        lua_pushcfunction(L, i2s_tx_handle_write);
        lua_setfield(L, -2, "write");

        lua_pushcfunction(L, i2s_tx_handle_stop);
        lua_setfield(L, -2, "stop");
    }
    lua_pop(L, 1);

    /* Module table */
    lua_newtable(L);

    lua_pushcfunction(L, i2s_tx_new);
    lua_setfield(L, -2, "new");

    return 1;
}

esp_err_t lua_driver_i2s_tx_register(void)
{
    return cap_lua_register_module("i2s_tx", luaopen_i2s_tx);
}
