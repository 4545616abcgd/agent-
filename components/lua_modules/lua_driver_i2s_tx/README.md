# Lua I2S TX (Audio Output)

This module provides I2S digital audio output from Lua. It wraps
ESP-IDF's legacy I2S driver for audio playback via MAX98357A or
similar I2S DAC/amplifiers.

## How to call

- Import it with `local i2s_tx = require("i2s_tx")`
- Open a speaker with `local spk = i2s_tx.new(config)`
- `spk:play(data)` — play audio samples
- `spk:close()` — release the I2S peripheral
