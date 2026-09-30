# 智能桌面气象站 RTC Bot 配置

融合固件的网页问答继续使用 ESP-Claw Root Agent；火山 RTC Bot 使用云端
LLM。两者调用同一设备上的 ESP-Claw capability，但提示词和工具声明分别配置。
此文件是云端配置清单，不会在固件启动时自动修改火山控制台。

## 烧录前确认

融合工程的 `sdkconfig` 需要在本机配置 Instance ID、Product Key、
Product Secret、Device Name 和 Bot ID；这些值不能写入
`sdkconfig.defaults`、文档或 Git。仅编译成功不代表设备已具备云端注册条件。
网页问答与 RTC 语音共享下面的设备 capability 和天气缓存，但目前分别由
ESP-Claw Root Agent 与火山 Bot 管理对话历史，不承诺跨通道续聊。

## 身份提示词

将以下内容设置到当前硬件对话智能体的系统提示词：

> 你是这台智能桌面气象站的语音助手。产品面向家庭和办公场景，
> 以天气信息、环境感知和自然交互帮助用户安排日常生活。ESP-Claw 是设备的
> 本地能力框架，火山引擎提供实时语音通道。用户问“你是谁”时，简洁地介绍
> 自己是智能桌面气象站助手，不要自称通用聊天机器人。询问当前天气、预报、
> 预警、设备状态或时间时，必须调用对应工具，再根据工具返回结果回答。
> 不编造实时气象数据、传感器读数、设备功能或预警；数据缺失或过期时明确说明。
> 天气工具只查询设备已经配置的地点，不支持临时查询其他城市；遇到这种
> 请求要说明限制，不能把设备地点的天气说成其他城市的天气。户外天气与室内
> 传感器读数是两回事。工具报错时如实说明，不要用模型常识冒充实时数据。
> 使用自然、简洁的中文回答。

## Function Calling

在火山硬件对话智能体中声明下面七个客户端工具。工具名必须逐字一致；
参数是 JSON object。当前设备端只放行这些查询工具，不放行重启、配置修改、
文件或网络操作。

完整的工具名称、描述和参数定义见同目录 `rtc_bot_tools.json`。
这是 OpenAI 风格的工具定义参考，不是已经验证可直接提交的火山 API 请求体；
应按硬件智能体控制台要求逐项配置客户端工具，不要配置成云端 HTTP 工具。
所有参数都禁止未声明字段，尤其不要给天气工具传 `location`。
配置必须保存/发布到固件当前 Bot ID 对应的智能体，再退出旧 RTC 房间并重新唤醒。
仅修改本地 Skill 或这份文件不会改变云端 Bot。

| 工具名 | 用途 | 参数 schema |
| --- | --- | --- |
| `weather_get_current` | 当前天气 | `{"type":"object","properties":{}}` |
| `weather_get_hourly` | 逐小时预报 | `{"type":"object","properties":{"hours":{"type":"integer","minimum":1,"maximum":24}}}` |
| `weather_get_daily` | 逐日预报 | `{"type":"object","properties":{"days":{"type":"integer","minimum":1,"maximum":7}}}` |
| `weather_get_alerts` | 天气预警 | `{"type":"object","properties":{"include_details":{"type":"boolean"}}}` |
| `weather_get_status` | 天气数据源和缓存状态 | `{"type":"object","properties":{}}` |
| `get_system_info` | 设备、内存、Wi-Fi 和 IP 状态 | `{"type":"object","properties":{"sections":{"type":"array","items":{"type":"string","enum":["chip","uptime","version","memory","cpu","wifi","ip"]},"minItems":1,"uniqueItems":true}}}` |
| `get_current_time` | 当前本地时间 | `{"type":"object","properties":{}}` |

设备收到 RTC 的 `tool` 消息后，调用 ESP-Claw capability；当前固件以
`func` 二进制消息回传 `ToolCallID` 和 `Content`。回包字段和云端消费行为
仍需用硬件智能体的实际会话验证。请在串口确认每次调用的
`dispatch=ESP_OK`、`send=ESP_OK`，再检查 Bot 是否根据结果回答。
启动时 `local probe` 仅验证设备端真实能力和 Agent 权限，绝不会发给云端。
`tool announced` 仅表示云端开始准备调用，不能执行工具；收到 `tool received`
才有真实参数。`RTC message submitted` 的 `msg_id` 可与 `RTC message receipt`
对应，回执 `error=0` 只表示传输成功，仍不证明 Bot 已使用结果。
仅看到 `tool_calls` 或发送成功，不代表工具闭环成功。

云端配置完成后，先用“你是谁”“现在几点”“设备联网了吗”“当前天气”
四个问题逐项验收。天气结果依赖和风天气配置与缓存；未接入的本地传感器
不能作为已支持能力介绍。

## 融合验收

1. 在网页和语音分别问当前天气、明天预报，核对地点、数值和缓存时间；
   两端应读取同一套 `weather_service`，不能由 Bot 猜测天气。
2. 在网页和语音分别问设备状态；语音串口应出现相应工具的
   `dispatch=ESP_OK send=ESP_OK`，且 Bot 应在工具结果返回后再回答。
3. 暂停天气网络或清空配置后再询问，两个通道都应说明数据不可用或过期。
4. 连续语音测试时确认只有 RTC 占用 I2S，网页仍能文字回答；同时检查
   用户 ASR 不会复读扬声器内容。
5. 合并工程使用 ESP-Claw 的双 OTA/FATFS 分区，独立 RTC 工程使用另一套
   factory/model 地址。未制定完整迁移和数据保留方案前，不要按旧地址
   `0x20000` 单独烧录合并应用，也不要擦除 NVS 或模型。

## 2026-09-30 工程核对记录

以下是工程调试记录，不是产品全部功能已完成的声明。

| 目标 | 当前实现与证据 | 剩余验收 |
| --- | --- | --- |
| ESP-Claw 是产品主体 | `edge_agent` 启动 Root Agent、路由器、调度器，实机有 `Root agent ready` | 网页真实问答与 RTC 同时运行的压力测试 |
| 火山硬件实时语音保留 | 共用独立 RTC 工程的音频、Agent 和消息源码；此前已验证远端入房、上下行音频 | 回声、连续对话和打断还需要声学实测 |
| 实时语音以能力和 Skill 接入 | `cap_volc_rtc` 有状态/启动/停止接口；实机读取 `/system/skills/realtime_voice/skill.md` | 网页激活 Skill 并启动/停止语音的完整交互 |
| 网页和语音共享天气工具 | bridge 直接调用 `claw_cap_call`，不复制天气 HTTP 实现；实机 Agent 权限下天气/系统只读自检通过 | 火山 Bot 发出真实调用并按结果回答 |
| 工具协议和错误回传 | 对照官方 Embedded Kit Function Calling 示例；25 项主机检查通过 | 云端工具声明、消息回执及 Bot 使用结果 |
| 产品身份统一 | 网页固件有气象站人设；云端人设清单在本文件 | 当前 Bot 的控制台配置保存/发布和“你是谁”实测 |
| 天气缓存时间正确 | 已修复校时前抓取导致的假过期；主机回归通过，修复版冷启动实测有效 epoch、`cache_age_sec=19`、`stale=false` | 断网后的缓存过期及校时异常测试 |
| 原 ESP-Claw Memory/Skills/Lua | 模块仍存在，SYSTEM/DATA Skill 都被扫描 | 自动记忆提取目前在 `app_claw.c` 中被调试设置禁用，后续需恢复并测资源占用 |
| 网页回答从扬声器播放 | RTC 配置下旧网页 TTS 已禁用，避免争用 I2S | 尚未实现经硬件智能体 TTS 的网页播音桥接 |

语音会话的思考者是火山 Bot，网页会话的思考者是 ESP-Claw Root Agent；
共享的是设备能力和天气缓存，不是 LLM、Memory 或完整对话历史。
目前只向语音侧放行七个只读工具，不能据此声称全部 Lua、文件、MCP、
配置修改或设备控制工具都已开放。

RTC 目前通过 `edge_agent/components/volc_rtc_voice` 编译引用
`volc_rtc_agent/main` 的同一份源码，避免音频实现分叉；这是可用的共享边界，
后续仍可整理成顶层公共组件，但不应在工具闭环未验收时同时大范围搬迁文件。

天气服务现在需要有效网络时间才启动抓取。离线、未校时或无缓存时应明确
说明不可用；不能用模型常识替代真实天气。此次只读自检不清除天气配置。

本次修复版在 COM14 仅写入核对过的 `ota_0` 应用分区并通过写入哈希校验。
新固件恢复唤醒启动，关闭诊断用开机自动入房。天气只读自检实际通过 Agent
权限调用，缓存包含当前天气、24 小时预报、7 天预报及有效预警结果。
设备状态只读自检返回真实 uptime 和内存信息；这不是火山云端工具调用。
电脑所在网络暂不能访问设备热点网段，因此网页端问答还没有完成本轮实测。
