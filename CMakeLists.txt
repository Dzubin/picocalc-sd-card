# PicoCalc-SD-Drive (by Thomas Dzubin)
#
# Makes the PicoCalc look like a USB drive to a PC, using the PicoCalc's SD card
# as the disc. Builds one firmware, named with the chip:
#
#   PicoCalc-SD-Drive-RP2040.uf2 / PicoCalc-SD-Drive-RP2350.uf2
#
# copied to the top-level folder so the two builds can sit side by side.
# The picocalc-text-starter drivers (by Blair Leduc) are vendored, read-only, in
# picocalc-text-starter-main/ and are used as they are.

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in the Pico SDK (must be before project())
include(pico_sdk_import.cmake)

project(PicoCalc-SD-Drive C CXX ASM)

pico_sdk_init()

# The version number lives in PicoCalc-SD-Drive.h (one place); the firmware reports it too.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/PicoCalc-SD-Drive.h) # a version bump re-runs this
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/PicoCalc-SD-Drive.h _version_line REGEX "define SD_DRIVE_VERSION ")
string(REGEX MATCH "V[0-9]+[.][0-9]+[A-Z]*" SD_DRIVE_VERSION_TEXT "${_version_line}")

# The vendored starter drivers: the SD card, LCD (with its fonts), keyboard and the
# south bridge the keyboard talks to.
set(STARTER ${CMAKE_CURRENT_LIST_DIR}/picocalc-text-starter-main/drivers)

add_executable(PicoCalc-SD-Drive
    PicoCalc-SD-Drive.c
    ${STARTER}/sdcard.c
    ${STARTER}/lcd.c
    ${STARTER}/font-8x10.c
    ${STARTER}/font-5x10.c
    ${STARTER}/keyboard.c
    ${STARTER}/southbridge.c
)

pico_set_program_name(PicoCalc-SD-Drive "PicoCalc-SD-Drive")
pico_set_program_version(PicoCalc-SD-Drive "${SD_DRIVE_VERSION_TEXT}")

# The USB port belongs to the drive, so the SDK's own stdio over USB stays off.
pico_enable_stdio_uart(PicoCalc-SD-Drive 0)
pico_enable_stdio_usb(PicoCalc-SD-Drive 0)

target_compile_options(PicoCalc-SD-Drive PRIVATE -Wall)

target_include_directories(PicoCalc-SD-Drive PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${STARTER}
)

target_link_libraries(PicoCalc-SD-Drive
    pico_stdlib
    pico_unique_id
    pico_bootrom
    pico_multicore
    hardware_spi
    hardware_i2c
    hardware_gpio
    hardware_irq
    tinyusb_device
)

# The chip is part of the output file name, so a file always says which board it
# was built for.
if(PICO_PLATFORM MATCHES "^rp2040")
    set(CHIP_NAME RP2040)
elseif(PICO_PLATFORM MATCHES "^rp2350")
    set(CHIP_NAME RP2350)
else()
    set(CHIP_NAME ${PICO_PLATFORM})
endif()
set_target_properties(PicoCalc-SD-Drive PROPERTIES OUTPUT_NAME PicoCalc-SD-Drive-${CHIP_NAME})

pico_add_extra_outputs(PicoCalc-SD-Drive)

# Copy the finished .uf2 (same name) to the top-level project folder.
add_custom_command(TARGET PicoCalc-SD-Drive POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE_DIR:PicoCalc-SD-Drive>/$<TARGET_FILE_BASE_NAME:PicoCalc-SD-Drive>.uf2
            ${CMAKE_CURRENT_LIST_DIR}/
)
