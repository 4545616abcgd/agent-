idf_component_register(
    SRCS "voice_duplex_volc.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_websocket_client
        mbedtls
        heap
        freertos
        log
        esp_hw_support
        claw_cap
        json
)
