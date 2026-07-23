# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

add_compile_definitions(ON_ESP32)
add_compile_definitions(PFB_QUEUE_NO_THREAD_SAFETY)  # Disable PFBQueue mutex locking to remove deadlocks
# Compile-time flag for the optional GC9A01 round display (see peripherals/display/).
# Comment this line out to build the default image with no display code (and no LovyanGFX
# linked in). The display and the W5500 Ethernet add-on share the aux SPI3 bus + GPIO47/48
# and are therefore mutually exclusive; comms_ethernet.cpp guards against both being built.
add_compile_definitions(WITH_DISPLAY)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(adsbee_esp)