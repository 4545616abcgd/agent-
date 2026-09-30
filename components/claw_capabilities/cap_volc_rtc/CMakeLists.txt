if(CONFIG_APP_VOLC_RTC_VOICE)
    set(CAP_VOLC_RTC_SRCS "src/cap_volc_rtc.c")
endif()

idf_component_register(
    SRCS ${CAP_VOLC_RTC_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES claw_cap volc_rtc_voice
)
