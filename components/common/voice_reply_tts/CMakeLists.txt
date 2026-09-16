idf_component_register(
    SRCS "voice_reply_tts.c"
    INCLUDE_DIRS "include"
    REQUIRES voice_tts voice_service voice_agent voice_audio heap
)
