# ESP-Claw voice_duplex_volc v0.2

第一阶段接入骨架。

目标：
WakeNet -> voice_audio -> voice_duplex_volc -> Doubao Duplex -> MAX98357A

当前包含：
- 组件结构
- WebSocket层入口
- Session入口
- PCM发送入口
- Event解析入口

下一步：
1. 对接官方Go Demo JSON事件
2. 完成WebSocket TLS
3. 完成session.create
4. 完成audio.delta播放
5. 接入ESP-Claw Tool
