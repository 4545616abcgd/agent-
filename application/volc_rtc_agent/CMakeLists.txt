cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{ADF_PATH})
    message(FATAL_ERROR "ADF_PATH is not set. Point it to the locked ESP-ADF checkout.")
endif()

list(APPEND EXTRA_COMPONENT_DIRS
    "$ENV{ADF_PATH}/components/esp-adf-libs/esp_audio_codec")

include($ENV{ADF_PATH}/CMakeLists.txt)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(volc_rtc_agent VERSION 0.1.0)
