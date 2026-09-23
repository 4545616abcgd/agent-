idf_component_register(
    SRCS
        "app_main.c"
        "rtc_agent.c"
        "rtc_audio.c"
        "rtc_message.c"
    INCLUDE_DIRS "."
    EMBED_FILES "assets/wake_ack_wozai.pcm"
    REQUIRES
        audio_recorder
        clouds
        esp-adf-libs
        esp_audio_codec
        esp_driver_i2s
        esp_event
        esp_http_client
        esp_netif
        esp_wifi
        json
        nvs_flash
        volc_conv_ai
)
