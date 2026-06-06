# i8088 slow-clock signal generator — RP2040 (Pico 1).
cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Reuse the vendored SDK + ARM-GCC 14.2 + picotool from the sibling magner150
# project, so nothing needs to be downloaded. Override -DREF_TOOLCHAIN=... to
# point elsewhere, or set PICO_SDK_PATH in the environment and delete these
# FORCEs to use your own SDK install.
set(REF_TOOLCHAIN "/Users/andreykirillov/Desktop/projects/mcu/pico/projects/c/magner150_vga_rp2350/toolchain"
    CACHE PATH "Folder holding pico-sdk-2.2.0 / 14_2_Rel1 / picotool")

set(PICO_TOOLCHAIN_PATH "${REF_TOOLCHAIN}/14_2_Rel1" CACHE PATH "arm-none-eabi toolchain" FORCE)
set(PICO_SDK_PATH       "${REF_TOOLCHAIN}/pico-sdk-2.2.0" CACHE PATH "pico-sdk" FORCE)
set(picotool_DIR        "${REF_TOOLCHAIN}/picotool/picotool" CACHE PATH "prebuilt picotool" FORCE)
set(PICO_BOARD pico CACHE STRING "Board type (Pico 1 / RP2040)")

include(pico_sdk_import.cmake)

project(clkgen C CXX ASM)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

pico_sdk_init()

add_executable(clkgen main.c lcd1602_i2c.c)

pico_set_program_name(clkgen "clkgen")
pico_set_program_version(clkgen "1.0")

# USB stdio ON so `picotool load -fx -u` can reset the running chip into BOOTSEL
# over the USB cable (no BOOTSEL button press). Also gives printf debug.
pico_enable_stdio_uart(clkgen 0)
pico_enable_stdio_usb(clkgen 1)

target_link_libraries(clkgen
        pico_stdlib
        pico_multicore
        hardware_adc
        hardware_i2c)

target_include_directories(clkgen PRIVATE ${CMAKE_CURRENT_LIST_DIR})

pico_add_extra_outputs(clkgen)
