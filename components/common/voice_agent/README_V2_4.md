# Voice Agent V2.4

Realtime-only voice control path:

WakeNet -> voice_service -> voice_agent -> realtime_session -> volc backend -> voice_duplex_volc.

The old ASR -> text agent -> local TTS path is no longer registered as the wake-word fallback.
The compatibility ASR settings API remains in voice_dialog for the existing web configuration UI.
