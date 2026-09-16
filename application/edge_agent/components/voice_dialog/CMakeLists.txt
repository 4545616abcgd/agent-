idf_component_register(
    SRCS "voice_dialog.c"
    INCLUDE_DIRS "include"
    REQUIRES
        voice_service
        voice_agent
        voice_reply_tts
        settings
        esp_timer
)
