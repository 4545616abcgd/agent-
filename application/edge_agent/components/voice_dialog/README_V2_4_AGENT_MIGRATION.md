# Voice Dialog V2.4

`voice_dialog` is now a compatibility adapter only.

Wake-word voice traffic is routed exclusively to `voice_agent` / Doubao Realtime.
The legacy SiliconFlow ASR -> ESP-Claw text agent -> local ESP-TTS handoff is not
registered anymore. The ASR settings getter/setter remain for compatibility with
the existing HTTP configuration API.
