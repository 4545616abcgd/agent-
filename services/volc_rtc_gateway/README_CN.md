# ESP-Claw 火山 RTC 会话网关

这个小服务把长期凭据留在电脑或云服务器上，为 ESP32 签发短期 RTC
Token，并调用火山引擎 `StartVoiceChat`。固件只能拿到当前房间所需的
`AppID / RoomID / UserID / Token`，不会接触 RTC AppKey 或火山 AK/SK。

当前依据：

- 火山官方 `rtc-aigc-demo` commit
  `47835c0343d0aaf37db6813169b62ba395a56bff`
- VoiceChat OpenAPI `2024-12-01`（匹配“实时对话式 AI”应用）
- Node.js 22

`2025-06-01` 仅适用于另一项“AI 音视频互动方案”商品。两类应用不可混用，
否则 `StartVoiceChat` 会返回 `NoPermissionForApp`。

## 开发阶段：电脑充当后端

电脑和 ESP32 连接同一个 Wi-Fi。首次运行：

```powershell
cd D:\esp-claw\services\volc_rtc_gateway
Copy-Item .env.example .env.local
npm ci
```

在 `.env.local` 填写：

- `RTC_APP_ID`、`RTC_APP_KEY`
- `VOLCENGINE_ACCESS_KEY_ID`、`VOLCENGINE_SECRET_ACCESS_KEY`
- `ASR_APP_ID`、`ASR_ACCESS_TOKEN`（[豆包流式语音识别](https://console.volcengine.com/speech/service/16)）
- `TTS_APP_ID`、`TTS_ACCESS_TOKEN`（[豆包语音合成](https://console.volcengine.com/speech/service/8)）
- `ARK_ENDPOINT_ID`（[方舟在线推理接入点](https://console.volcengine.com/ark/region:ark+cn-beijing/endpoint?config=%7B%7D)，不是模型名称）
- 随机生成的 `DEVICE_API_KEY`，至少 24 个字符

启动：

```powershell
npm start
```

查看电脑局域网 IPv4 地址：

```powershell
Get-NetIPAddress -AddressFamily IPv4 |
  Where-Object { $_.IPAddress -notlike '127.*' -and $_.PrefixOrigin -ne 'WellKnown' }
```

ESP32 后续访问 `http://<电脑局域网IP>:8080`。首次测试如被 Windows
防火墙阻止，只放行 TCP 8080 的专用网络入站连接，不要把端口暴露到公网。

健康检查：

```powershell
Invoke-RestMethod http://127.0.0.1:8080/health
```

创建设备会话：

```powershell
$headers = @{ Authorization = 'Bearer <DEVICE_API_KEY>' }
$body = @{ device_id = 'espclaw-aabbccddeeff' } | ConvertTo-Json
Invoke-RestMethod -Method Post -Headers $headers -ContentType application/json `
  -Body $body http://127.0.0.1:8080/v1/rtc/sessions
```

停止会话：

```powershell
Invoke-RestMethod -Method Post -Headers $headers `
  http://127.0.0.1:8080/v1/rtc/sessions/<session_id>/stop
```

## 配置对话

`config/voice_chat.json` 使用火山官方当前 VoiceChat 字段。默认配置为：

- 豆包实时语音链路
- 云端 VAD，连续多轮
- `800 ms` 句尾静音判定，不再固定录制 3 秒
- 支持用户打断
- 入房欢迎语为“我在。”
- 保留最近 10 轮上下文
- 同一设备的会话创建/停止串行执行，设备重试不会并发创建两个 VoiceChat 任务
- 房间 ID 使用火山嵌入式 Demo 的 `OPUS` 前缀，以匹配 RTC Lite
  的 16 kHz / 32 kbps Opus 传输策略
- 已在[硬件场景配置](https://console.volcengine.com/rtc/aigc/cloudRTC)启用对应的
  RTC 应用和 `OPUS` 房间规则

RTC AppID/AppKey、OpenAPI AK/SK 只负责 RTC 入房和调用 `StartVoiceChat`。
ASR、TTS 和方舟推理点各自还需要上面的运行时凭据；缺少这些值时，
`StartVoiceChat` 可能已被接收，但机器人不会发送欢迎语或语音回复。

火山控制台“快速跑通 Demo”生成的 VoiceChat 配置可以覆盖这个文件，但不要
在 JSON 中写入 `AppId`、`RoomId` 或 `TaskId`，这些字段由网关动态生成。

当前配置尚未声明 ESP-Claw 天气/网页工具。等火山账号与实际 Function Calling
配置到位后，再把工具 schema 加入此文件，并把设备侧
`rtc_message_dispatch_tool()` 接到 capability registry；在结果回传协议实测通过前，
不要把“收到 tool_calls 日志”误认为工具闭环已完成。

运行不依赖云端凭据的测试：

```powershell
cd D:\esp-claw\services\volc_rtc_gateway
npm test
```

## 部署

硬件链路稳定后可使用附带的 `Dockerfile` 部署到火山引擎云服务器或容器
服务。公网部署必须使用 HTTPS，并在平台的 Secret/环境变量管理中保存所有
长期凭据。不要把 `.env.local` 加入 Git。
