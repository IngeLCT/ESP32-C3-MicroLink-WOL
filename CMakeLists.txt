cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.credentials")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.credentials")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_c3_microlink_wol VERSION 1.0.0)
