# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

add_compile_definitions(ON_ESP32)
add_compile_definitions(PFB_QUEUE_NO_THREAD_SAFETY)  # Disable PFBQueue mutex locking to remove deadlocks
# Compile-time flag for the optional GC9A01 round display (see peripherals/display/).
# OFF by default: the standard image ships with no display code (and no LovyanGFX linked in).
# Enable with the build.sh --display flag (or WITH_DISPLAY=1 env var), which passes
# -D WITH_DISPLAY=ON to this build; or configure cmake/idf.py directly with -D WITH_DISPLAY=ON.
# The display and the W5500 Ethernet add-on share the aux SPI3 bus + GPIO47/48 and are therefore
# mutually exclusive; comms_ethernet.cpp guards against both being built.
option(WITH_DISPLAY "Build the optional GC9A01 round display support" OFF)
if(WITH_DISPLAY)
    add_compile_definitions(WITH_DISPLAY)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(adsbee_esp)