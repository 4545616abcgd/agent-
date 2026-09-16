# Lua I2S (Audio Input)

This module provides I2S digital microphone input from Lua. It wraps
ESP-IDF's legacy I2S driver so you can read audio samples from INMP441
or similar I2S MEMS microphones.

## How to call

- Import it with `local i2s = require("i2s")`
- Open a microphone with `local mic = i2s.new(config)`
  - `config` is a Lua table with the following fields:
    - `bclk_pin` (required): GPIO for I2S bit clock (SCK on INMP441)
    - `lrck_pin` (required): GPIO for I2S word select (WS on INMP441)
    - `data_in_pin` (required): GPIO for I2S data input (SD on INMP441)
    - `sample_rate` (optional): sample rate in Hz, default `16000`
    - `bits_per_sample` (optional): `8`, `16`, `24`, or `32`, default `24`
    - `channel` (optional): `"left"` or `"right"`, default `"left"`
    - `dma_buf_count` (optional): number of DMA buffers, default `8`
    - `dma_buf_len` (optional): samples per DMA buffer, default `64`
    - `use_apll` (optional): use APLL for accurate clock, default `true`
    - `port` (optional): I2S port number (`0` or `1`), default `0`
- `mic:read()` → a single `int32_t` audio sample (blocking)
- `mic:read_n(count)` → a Lua table of `count` samples (blocking)
- `mic:read_raw(count)` → a binary string of raw `int32_t` LE bytes (blocking)
- `mic:close()` — releases the I2S peripheral. Idempotent.
- `i2s.calc_rms(samples)` → RMS amplitude (float). Accepts a table or raw string.
- `i2s.calc_db(samples [, full_scale])` → dBFS value (float). Default full-scale is 2^23 for 24-bit.

## Example: INMP441 microphone

```lua
local i2s = require("i2s")

-- Wiring: SCK→bclk_pin, WS→lrck_pin, SD→data_in_pin, L/R→GND
local mic = i2s.new({
    bclk_pin       = 16,
    lrck_pin       = 17,
    data_in_pin    = 15,
    sample_rate    = 16000,
    bits_per_sample = 24,
    channel        = "left",
})

-- Read 1 second of audio
local samples = mic:read_n(16000)
local db = i2s.calc_db(samples)
print(string.format("Volume: %.2f dBFS", db))

mic:close()
```

## Notes

- The driver uses ESP-IDF's **legacy** I2S driver (`driver/i2s.h`).
- INMP441's L/R pin must be tied to GND (left channel) or VDD (right channel).
- All read operations are **blocking** — they wait until the requested data
  is available. There is no callback or interrupt interface in this module.
- For continuous streaming, use a separate Lua thread with `read_n()` or
  `read_raw()` in a polling loop.
