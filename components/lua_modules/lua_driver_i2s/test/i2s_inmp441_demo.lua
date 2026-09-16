-- ============================================================================
-- INMP441 I2S MEMS 麦克风测试脚本 (for ESP-Claw with lua_driver_i2s)
-- ============================================================================
-- 接线：
--   INMP441 VDD  → ESP32 3.3V
--   INMP441 GND  → ESP32 GND
--   INMP441 SCK  → GPIO16 (BCLK)
--   INMP441 WS   → GPIO17 (LRCK)
--   INMP441 SD   → GPIO15 (DOUT)
--   INMP441 L/R  → GND (左声道)
-- ============================================================================

local i2s = require("i2s")
local delay = require("delay")

-- 配置（根据实际接线修改 GPIO）
local mic = i2s.new({
    bclk_pin       = 16,   -- SCK
    lrck_pin       = 17,   -- WS
    data_in_pin    = 15,   -- SD
    sample_rate    = 16000,
    bits_per_sample = 24,
    channel        = "left",
    dma_buf_count  = 8,
    dma_buf_len    = 64,
})

print("=== INMP441 初始化成功 ===")
print("采样率: 16000 Hz")
print("位深: 24bit")
print("")

-- ============================================================================
-- 测试 1: 单次采样
-- ============================================================================
print("--- 测试 1: 单次采样 ---")
local sample = mic:read()
print(string.format("单次采样值: %d", sample))

-- ============================================================================
-- 测试 2: 批量读取 1024 个采样点并计算音量
-- ============================================================================
print("")
print("--- 测试 2: 批量读取 + 音量计算 ---")
local data = mic:read_n(1024)
print(string.format("读取了 %d 个采样点", #data))

local rms = i2s.calc_rms(data)
local db = i2s.calc_db(data)
print(string.format("RMS 音量 = %.2f", rms))
print(string.format("dBFS     = %.2f dB", db))

-- 找峰值
local max_val = 0
local min_val = 0
for i = 1, #data do
    local s = data[i]
    if s > max_val then max_val = s end
    if s < min_val then min_val = s end
end
print(string.format("峰值: max=%d, min=%d, 动态范围=%d", max_val, min_val, max_val - min_val))

-- ============================================================================
-- 测试 3: read_raw 读取（返回 binary string，省内存）
-- ============================================================================
print("")
print("--- 测试 3: read_raw (binary string) ---")
local raw = mic:read_raw(512)
print(string.format("原始数据长度: %d 字节 = %d 个采样点", #raw, #raw / 4))

local rms2 = i2s.calc_rms(raw)
local db2 = i2s.calc_db(raw)
print(string.format("(raw) RMS = %.2f, dB = %.2f dBFS", rms2, db2))

-- ============================================================================
-- 测试 4: 简单语音活动检测——静音时 vs 说话时
-- ============================================================================
print("")
print("--- 测试 4: 语音活动检测 ---")
print("拍手或说话看看音量变化...")

for i = 1, 5 do
    local buf = mic:read_n(1600)  -- 100ms 的数据
    local level = i2s.calc_rms(buf)
    local db_level = i2s.calc_db(buf)
    local status = "静音"
    if level > 1000000 then status = "说话中" end
    if level > 3000000 then status = "大声" end
    print(string.format("  [%d/5] RMS=%.0f  dB=%.1f  (%s)", i, level, db_level, status))
    delay.ms(100)
end

-- ============================================================================
-- 测试 5: 关闭
-- ============================================================================
print("")
print("--- 测试 5: 关闭 ---")
mic:close()
print("I2S 已关闭，测试完成")
