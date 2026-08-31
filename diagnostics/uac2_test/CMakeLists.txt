cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2)
set(PICO_PLATFORM rp2350)
set(PICO_TINYUSB_PATH ${CMAKE_CURRENT_LIST_DIR}/../../lib/tinyusb)
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(uac2_test C CXX ASM)
pico_sdk_init()
add_executable(uac2_test
  ${CMAKE_CURRENT_LIST_DIR}/../../lib/tinyusb/examples/device/uac2_headset/src/main.c
  ${CMAKE_CURRENT_LIST_DIR}/../../lib/tinyusb/examples/device/uac2_headset/src/usb_descriptors.c)
target_include_directories(uac2_test PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../lib/tinyusb/examples/device/uac2_headset/src
  ${CMAKE_CURRENT_LIST_DIR}/../../lib/tinyusb/hw)
target_include_directories(uac2_test PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../lib/tinyusb/src
  ${CMAKE_CURRENT_LIST_DIR}/../../lib/tinyusb/hw/bsp/rp2040)
target_compile_definitions(uac2_test PRIVATE CFG_TUSB_MCU=OPT_MCU_RP2040)
target_link_libraries(uac2_test pico_stdlib pico_multicore tinyusb_device tinyusb_board)
pico_add_extra_outputs(uac2_test)
