cmake_minimum_required(VERSION 3.16)

# Este firmware solo soporta ESP32-C3. También está declarado en
# sdkconfig.defaults para que funcione igual desde idf.py y desde IDEs.
if(NOT DEFINED IDF_TARGET)
    set(IDF_TARGET "esp32c3")
endif()

set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.credentials")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.credentials")
endif()

set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_c3_alexa_wol VERSION 2.0.0)

idf_build_set_property(
    CXX_COMPILE_OPTIONS
    "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H"
    APPEND
)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(
    COMPILE_OPTIONS
    "-Wno-format-nonliteral;-Wno-format-security"
    APPEND
)
