# ESP-Claw 火山硬件智能体语音验证应用

这个独立应用用于先验证 ESP32-S3 上的连续多轮语音，再把 transport 接回
`application/edge_agent`。它不会删除或替换 ESP-Claw、天气服务和 Web 工具。

## 当前链路

```text
你好小益
  -> ESP-SR WakeNet / AFE / AEC
  -> 火山 ConversationalAI Embedded Kit 2.0
  -> 8 kHz G711A RTC 持续上行
  -> 豆包云端 VAD、LLM、TTS
  -> G711A 下行解码为 16 kHz PCM
  -> MAX98357A
```

当前不再使用本地 RTC 网关、手工 Token、固定三秒录音和旧
`StartVoiceChat` 调用。唤醒后由官方 SDK 自动完成设备注册、RTC 配置和智能体
入房；会话内持续上传音频，句尾和下一轮由云端 VAD 处理。

## 固定环境

- 芯片：ESP32-S3-WROOM-1-N16R8
- Flash：16 MB
- PSRAM：8 MB Octal
- ESP-IDF：5.5.4
- ESP-ADF：`d6e1ef5ccf29ca52dcb8332a3232505366755012`
- ConversationalAI Embedded Kit 2.0：
  `2c94f96f3aad4094e0e818cbb031149fd4384ead`
- I2S BCLK：GPIO4
- I2S WS/LRCLK：GPIO5
- INMP441 DATA：GPIO6，默认左声道
- MAX98357A DIN：GPIO17

官方 SDK 的来源和本地安全修改记录在
`components/volc_conv_ai/UPSTREAM.md`。

## 本地凭据

需要在火山“对话式 AI 硬件”产品详情页取得五项：

1. InstanceID
2. ProductKey
3. ProductSecret
4. DeviceName：`espclaw-dcb4d905d268`
5. 关联智能体的 Bot ID

使用 `idf.py menuconfig`，进入 `ESP-Claw RTC Agent` 填写。值只会写入本地
`sdkconfig`；该文件已被仓库忽略。不要把 ProductSecret 放进
`sdkconfig.defaults`、源码、聊天或 Git。

```powershell
$env:ADF_PATH='D:\esp-adf'
$env:IDF_PATH='D:\.espressif\.espressif\v5.5.4\esp-idf'
$env:IDF_TOOLS_PATH='C:\Espressif'
$env:IDF_PYTHON_ENV_PATH='D:\software\Espressif\python_env\idf5.5_py3.11_env'
$env:IDF_PYTHON_CHECK_CONSTRAINTS='no'
$env:IDF_TARGET='esp32s3'
$env:ESP_ROM_ELF_DIR='C:\Espressif\tools\esp-rom-elfs\20241011'
$env:PATH='C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\ccache\4.12.1;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;' + $env:PATH

cd D:\esp-claw\application\volc_rtc_agent
& "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" `
  "$env:IDF_PATH\tools\idf.py" -B build-idf554-s3 menuconfig
```

设备注册签名依赖真实时间。Wi-Fi 连接后应用先通过 SNTP 校时，校时成功后才
创建官方 SDK 实例，避免使用 1970 年时间导致注册失败。

## 构建和安全烧录

```powershell
& "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" `
  "$env:IDF_PATH\tools\idf.py" -B build-idf554-s3 build
```

2026-09-23 已完成无凭据编译验证：

- `volc_rtc_agent.bin`：1,879,184 字节
- factory app 分区：6 MiB
- 剩余：约 70%

测试阶段只写应用分区，不执行 `erase-flash`，也不覆盖分区表、NVS 或模型：

```powershell
& "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" -m esptool `
  --chip esp32s3 --port COM11 --baud 460800 `
  --before default_reset --after hard_reset `
  write_flash 0x20000 build-idf554-s3\volc_rtc_agent.bin
```

## 当前边界

- 已完成：官方硬件智能体 SDK 接入、设备注册入口、SNTP、连续 RTC 会话、
  G711A 上下行、全双工 I2S、AFE/AEC、WakeNet、“我在”应答、异步字幕和
  Tool 消息解析。
- 待真机验证：欢迎语、连续 20 轮、打断、30 分钟稳定性和 AEC 延迟标定。
- 待语音链路通过后接回：ESP-Claw 天气/Web capability 的执行结果回传，
  以及 Web 回答与 RTC 下行共用播放仲裁。

不要先把独立验证应用误当成完整 ESP-Claw 产品固件。天气和网页 Tool 的源码
仍在原工程中，等多轮语音验收通过后再接回，避免同时排查云端入房与工具协议。
