# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Enable ccache for fast compilation and instant rebuilds
set(CCACHE_ENABLE 1)
find_program(CCACHE_FOUND ccache HINTS "C:/Espressif/tools/ccache/4.12.1/ccache-4.12.1-windows-x86_64")
if(CCACHE_FOUND)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_FOUND}")
    message(STATUS "ccache enabled for faster recompilation: ${CCACHE_FOUND}")
endif()

# Include esp-matter if ESP_MATTER_PATH is set in environment
if(DEFINED ENV{ESP_MATTER_PATH})
    set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
    include(${ESP_MATTER_PATH}/project_include.cmake)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-matter-cw-ww-led)
