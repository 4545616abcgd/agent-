/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lua binding for ESP-IDF I2S TX (audio output via MAX98357A or similar).
 * Usage: local i2s_tx = require("i2s_tx")
 */
#pragma once

#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_i2s_tx(lua_State *L);
esp_err_t lua_driver_i2s_tx_register(void);

#ifdef __cplusplus
}
#endif
