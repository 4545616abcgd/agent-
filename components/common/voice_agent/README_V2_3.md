# ESP-Claw Voice Agent V2.3

本版本开始接入真实调用链。

目标:
voice_service
    |
voice_agent
    |
realtime_session
    |
volc backend
    |
Doubao realtime

保留:
- db_key
- model 1.2.6.1
- zh_female_xiaohe_jupiter_bigtts

下一步:
迁移现有 voice_duplex_volc 实现到 backend。
