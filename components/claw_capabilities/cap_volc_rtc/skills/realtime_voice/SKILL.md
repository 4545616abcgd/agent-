---
{
  "name": "realtime_voice",
  "description": "Start, stop, or check the device's live voice conversation when the user explicitly asks to speak with the weather-station assistant.",
  "metadata": {
    "cap_groups": ["cap_volc_rtc"],
    "manage_mode": "readonly",
    "category": ["utility"],
    "tags": ["voice", "rtc"]
  }
}
---

# Real-time Voice

The device's Volc hardware conversational agent handles speech recognition, live dialogue, and speech output while an RTC session is active. ESP-Claw remains the device and Web Agent, and supplies selected local weather and status tools to the voice agent. Web and voice conversations do not share a transcript.

## Available tools

- `rtc_voice_get_status`: inspect the voice service and RTC room state.
- `rtc_voice_start`: request a live voice session. The session may still fail while Wi-Fi, cloud registration, or the room connection is pending.
- `rtc_voice_stop`: end the current live voice session.

Call `rtc_voice_start` only when the user explicitly requests a live voice conversation. A local wake phrase can also start a session without the Web Agent. Do not start RTC merely because the user asks a weather question in Web chat.

For a Web weather question, use ESP-Claw's weather tools directly. During a live voice conversation, the Volc agent must call the registered client weather tools to obtain the same device weather cache; it must not invent current conditions. Do not claim that starting RTC transfers the Web chat history, that the Web reply is automatically spoken, or that ESP-Claw's Root Agent controls the live voice dialogue.
