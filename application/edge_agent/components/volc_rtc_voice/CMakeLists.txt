if(DEFINED ENV{ADF_PATH})
    set(RTC_REQUIRES
        audio_recorder
        claw_cap
        esp-adf-libs
        esp_audio_codec
        esp_driver_i2s
        esp_event
        esp_netif
        esp_wifi
        json
        nvs_flash
        volc_conv_ai
        voice_service)
endif()

if(CONFIG_APP_VOLC_RTC_VOICE)
    if(NOT DEFINED ENV{ADF_PATH})
        message(FATAL_ERROR "APP_VOLC_RTC_VOICE requires ADF_PATH")
    endif()

    set(RTC_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../volc_rtc_agent/main")
    idf_component_register(
        SRCS
            "${RTC_SOURCE_DIR}/rtc_agent.c"
            "${RTC_SOURCE_DIR}/rtc_audio.c"
            "${RTC_SOURCE_DIR}/rtc_message.c"
            "rtc_tool_bridge.c"
        INCLUDE_DIRS "${RTC_SOURCE_DIR}"
        REQUIRES ${RTC_REQUIRES}
    )
else()
    idf_component_register(REQUIRES ${RTC_REQUIRES})
endif()
