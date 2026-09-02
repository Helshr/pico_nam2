cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_BOARD)
  set(PICO_BOARD pico2_w)
endif()
if(NOT DEFINED PICO_PLATFORM)
  set(PICO_PLATFORM rp2350)
endif()
set(PICO_CXX_ENABLE_EXCEPTIONS 1)
set(PICO_TINYUSB_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/tinyusb)  # vendored 0.20.0; SDK copy panics on UAC2

include(cmake/pico_sdk_import.cmake)
project(pico_nam C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

# Embed a tone: nam_set_model() runs the nam2c host tool (cmake/nam_model.cmake).
include(cmake/nam_model.cmake)
nam_set_model(example.nam)

# NAM inference engine (NeuralAmpModelerCore, a2_fast path)
set(NAMCORE ${CMAKE_CURRENT_LIST_DIR}/lib/NeuralAmpModelerCore)
file(GLOB NAMCORE_SRC ${NAMCORE}/NAM/*.cpp ${NAMCORE}/NAM/wavenet/*.cpp)
add_library(nam_engine STATIC ${NAMCORE_SRC})
target_include_directories(nam_engine BEFORE PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include/compat)
target_include_directories(nam_engine SYSTEM PUBLIC
  ${NAMCORE} ${NAMCORE}/Dependencies/eigen ${NAMCORE}/Dependencies/nlohmann)
target_compile_definitions(nam_engine PUBLIC
  NAM_SAMPLE_FLOAT=1 NAM_ENABLE_A2_FAST=1 NAM_USE_INLINE_GEMM=1 NAM_SHARED_PTR_ATOMIC_FREE_FUNCS=1)
target_compile_options(nam_engine PRIVATE -O2 -ffast-math -funroll-loops -Wno-unused-parameter)

# whole-archive so the engine's static-initializer JSON parsers survive linking
set(NAM_ENGINE_OPT -Wl,--whole-archive nam_engine -Wl,--no-whole-archive)
set(NAM_HW_LIBS pico_stdlib pico_multicore cmsis_core hardware_clocks hardware_vreg)

# Standalone WM8960 hardware test: generate a seamless I2S tone so the codec
# wiring can be verified before adding the full-duplex NAM pipeline.
add_executable(wm8960_test
  src/wm8960_test.c
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_tx.pio
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio
)
pico_generate_pio_header(wm8960_test ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_tx.pio)
pico_generate_pio_header(wm8960_test ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio)
# Do not add src/ here: it contains the NAM UAC2 tusb_config.h.  This
# standalone diagnostic must use Pico SDK's simple CDC-only USB configuration.
target_include_directories(wm8960_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR} include)
target_link_libraries(wm8960_test pico_stdlib hardware_i2c hardware_pio)
pico_enable_stdio_usb(wm8960_test 1)
pico_enable_stdio_uart(wm8960_test 0)
pico_add_extra_outputs(wm8960_test)

# WM8978 module bring-up: exact 12.288 MHz MCLK, a short I2S melody, then
# independent L2/R2 LINE IN analogue bypass to the headphone output.
add_executable(wm8978_test
  src/wm8978_test.c
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_tx.pio
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8978_mclk.pio
)
pico_generate_pio_header(wm8978_test ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_tx.pio)
pico_generate_pio_header(wm8978_test ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio)
pico_generate_pio_header(wm8978_test ${CMAKE_CURRENT_LIST_DIR}/src/wm8978_mclk.pio)
target_include_directories(wm8978_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR} include)
target_link_libraries(wm8978_test
  pico_stdlib hardware_clocks hardware_i2c hardware_pio)
pico_enable_stdio_usb(wm8978_test 1)
pico_enable_stdio_uart(wm8978_test 0)
pico_add_extra_outputs(wm8978_test)

# WM8978 line input with the embedded NAM2 model.  Unlike the USB UAC target,
# this target clocks the codec directly and uses DMA for full-duplex I2S so the
# dual-core NAM block can run without starving the PIO FIFOs.
add_executable(pico_wm8978_nam
  src/wm8978_nam.c
  src/nam_fx.cpp
  ${NAM_MODEL_SOURCE}
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8978_mclk.pio
)
pico_generate_pio_header(pico_wm8978_nam ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio)
pico_generate_pio_header(pico_wm8978_nam ${CMAKE_CURRENT_LIST_DIR}/src/wm8978_mclk.pio)
target_include_directories(pico_wm8978_nam PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/generated include src)
option(WM8978_NAM_SELF_TEST "Feed an internal 440 Hz tone into NAM instead of the codec ADC" OFF)
option(WM8978_LINE_BYPASS "Pass WM8978 ADC input directly to DAC for transport testing" OFF)
target_compile_definitions(pico_wm8978_nam PRIVATE NAM_OUTPUT_GAIN=0.65f)
if(WM8978_NAM_SELF_TEST)
  target_compile_definitions(pico_wm8978_nam PRIVATE NAM_SELF_TEST=1)
endif()
if(WM8978_LINE_BYPASS)
  target_compile_definitions(pico_wm8978_nam PRIVATE WM8978_LINE_BYPASS=1)
endif()
target_link_libraries(pico_wm8978_nam
  ${NAM_ENGINE_OPT} pico_stdlib pico_multicore cmsis_core
  hardware_clocks hardware_vreg hardware_i2c hardware_pio hardware_dma)
pico_enable_stdio_usb(pico_wm8978_nam 0)
pico_enable_stdio_uart(pico_wm8978_nam 0)
pico_add_extra_outputs(pico_wm8978_nam)

# Production-oriented WM8978 standalone NAM path. Unlike the earlier
# continuous-ring experiment it uses eight fixed 48-frame DMA slots. DMA is
# re-armed only from a completed-block interrupt, matching the complete-block
# ownership model of the proven USB NAM target.
add_executable(pico_wm8978_nam_blockqueue
  src/wm8978_nam_blockqueue.c
  src/nam_fx.cpp
  src/wm8978_usb_descriptors.c
  ${NAM_MODEL_SOURCE}
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8978_mclk.pio
)
pico_generate_pio_header(pico_wm8978_nam_blockqueue ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio)
pico_generate_pio_header(pico_wm8978_nam_blockqueue ${CMAKE_CURRENT_LIST_DIR}/src/wm8978_mclk.pio)
target_include_directories(pico_wm8978_nam_blockqueue PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/generated include src)
target_compile_definitions(pico_wm8978_nam_blockqueue PRIVATE
  # Match the reference render (input 0.5, model output 0.65).  The previous
  # 0.25 value made the embedded result over 8 dB quieter than the known-good
  # FretFlow reference and made comparison misleading.
  NAM_OUTPUT_GAIN=0.65f WM8978_CONTROL_USB=1)
option(WM8978_NAM_DISABLE_GATE
  "Process every non-overloaded sample for DI/reference regression" OFF)
if(WM8978_NAM_DISABLE_GATE)
  target_compile_definitions(pico_wm8978_nam_blockqueue PRIVATE
    WM8978_NAM_DISABLE_GATE=1)
endif()
target_link_libraries(pico_wm8978_nam_blockqueue
  ${NAM_ENGINE_OPT} pico_stdlib pico_multicore cmsis_core
  hardware_clocks hardware_vreg hardware_i2c hardware_pio hardware_dma tinyusb_device tinyusb_board)
pico_enable_stdio_usb(pico_wm8978_nam_blockqueue 0)
pico_enable_stdio_uart(pico_wm8978_nam_blockqueue 0)
pico_add_extra_outputs(pico_wm8978_nam_blockqueue)

# Same proven I2S diagnostic, configured for the CTIA headset microphone
# contact (RINPUT1).  This keeps the board-microphone ALC test reusable while
# providing a fixed-gain, dynamics-preserving guitar dry-through image.
add_executable(wm8960_guitar_test
  src/wm8960_test.c
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_tx.pio
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio
)
pico_generate_pio_header(wm8960_guitar_test ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_tx.pio)
pico_generate_pio_header(wm8960_guitar_test ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio)
target_include_directories(wm8960_guitar_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR} include)
target_compile_definitions(wm8960_guitar_test PRIVATE WM8960_USE_EXTERNAL_GUITAR=1)
target_link_libraries(wm8960_guitar_test pico_stdlib hardware_i2c hardware_pio)
pico_enable_stdio_usb(wm8960_guitar_test 1)
pico_enable_stdio_uart(wm8960_guitar_test 0)
pico_add_extra_outputs(wm8960_guitar_test)

# Controlled raw-input listening test: bypass software gain and gate so we
# can distinguish an analogue input noise problem from digital processing.
add_executable(wm8960_guitar_unity_test
  src/wm8960_test.c
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_tx.pio
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio
)
pico_generate_pio_header(wm8960_guitar_unity_test ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_tx.pio)
pico_generate_pio_header(wm8960_guitar_unity_test ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio)
target_include_directories(wm8960_guitar_unity_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR} include)
target_compile_definitions(wm8960_guitar_unity_test PRIVATE
  WM8960_USE_EXTERNAL_GUITAR=1
  WM8960_GUITAR_UNITY_MONITOR=1
)
target_link_libraries(wm8960_guitar_unity_test pico_stdlib hardware_i2c hardware_pio)
pico_enable_stdio_usb(wm8960_guitar_unity_test 1)
pico_enable_stdio_uart(wm8960_guitar_unity_test 0)
pico_add_extra_outputs(wm8960_guitar_unity_test)

# This feeds a known tone through the full-duplex PIO engine while discarding
# ADC samples, separating I2S scheduling errors from analogue-input noise.
add_executable(wm8960_duplex_tone_test
  src/wm8960_test.c
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_tx.pio
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio
)
pico_generate_pio_header(wm8960_duplex_tone_test ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_tx.pio)
pico_generate_pio_header(wm8960_duplex_tone_test ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio)
target_include_directories(wm8960_duplex_tone_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR} include)
target_compile_definitions(wm8960_duplex_tone_test PRIVATE
  WM8960_USE_EXTERNAL_GUITAR=1
  WM8960_DUPLEX_TONE_TEST=1
)
target_link_libraries(wm8960_duplex_tone_test pico_stdlib hardware_i2c hardware_pio)
pico_enable_stdio_usb(wm8960_duplex_tone_test 1)
pico_enable_stdio_uart(wm8960_duplex_tone_test 0)
pico_add_extra_outputs(wm8960_duplex_tone_test)

# One-shot raw ADC capture for analysing the analogue input noise spectrum.
add_executable(wm8960_adc_capture_test
  src/wm8960_test.c
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_tx.pio
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio
)
pico_generate_pio_header(wm8960_adc_capture_test ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_tx.pio)
pico_generate_pio_header(wm8960_adc_capture_test ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio)
target_include_directories(wm8960_adc_capture_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR} include)
target_compile_definitions(wm8960_adc_capture_test PRIVATE
  WM8960_USE_EXTERNAL_GUITAR=1
  WM8960_CAPTURE_DIAGNOSTIC=1
)
target_link_libraries(wm8960_adc_capture_test pico_stdlib hardware_i2c hardware_pio)
pico_enable_stdio_usb(wm8960_adc_capture_test 1)
pico_enable_stdio_uart(wm8960_adc_capture_test 0)
pico_add_extra_outputs(wm8960_adc_capture_test)

# USB level-only diagnostic for verifying whether the CTIA/pedal/cable chain
# actually reaches RINPUT1.  DAC output stays at digital zero while reporting.
add_executable(wm8960_guitar_level_test
  src/wm8960_test.c
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_tx.pio
  ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio
)
pico_generate_pio_header(wm8960_guitar_level_test ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_tx.pio)
pico_generate_pio_header(wm8960_guitar_level_test ${CMAKE_CURRENT_LIST_DIR}/src/wm8960_duplex.pio)
target_include_directories(wm8960_guitar_level_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR} include)
target_compile_definitions(wm8960_guitar_level_test PRIVATE
  WM8960_USE_EXTERNAL_GUITAR=1
  WM8960_GUITAR_LEVEL_DIAGNOSTIC=1
)
target_link_libraries(wm8960_guitar_level_test pico_stdlib hardware_i2c hardware_pio)
pico_enable_stdio_usb(wm8960_guitar_level_test 1)
pico_enable_stdio_uart(wm8960_guitar_level_test 0)
pico_add_extra_outputs(wm8960_guitar_level_test)

add_executable(pico_nam_loopback
  src/main.c src/nam_fx.cpp src/usb_descriptors.c src/led.c ${NAM_MODEL_SOURCE})
target_include_directories(pico_nam_loopback PRIVATE include src)
set(NAM_RUNTIME_SYS_KHZ 300000 CACHE STRING "Loopback system clock (kHz)")
target_compile_definitions(pico_nam_loopback PRIVATE NAM_RUNTIME_SYS_KHZ=${NAM_RUNTIME_SYS_KHZ})
option(NAM_SKIP_MODEL_INIT "Skip NAM model initialization for USB diagnostics" OFF)
if(NAM_SKIP_MODEL_INIT)
  target_compile_definitions(pico_nam_loopback PRIVATE NAM_SKIP_MODEL_INIT=1)
endif()
option(NAM_SKIP_CLOCK_SETUP "Keep the SDK default clocks for USB diagnostics" OFF)
if(NAM_SKIP_CLOCK_SETUP)
  target_compile_definitions(pico_nam_loopback PRIVATE NAM_SKIP_CLOCK_SETUP=1)
endif()
target_link_libraries(pico_nam_loopback ${NAM_ENGINE_OPT} ${NAM_HW_LIBS} tinyusb_device tinyusb_board)
pico_add_extra_outputs(pico_nam_loopback)

set(NAM_SYS_KHZ 300000 CACHE STRING "Benchmark system clock (kHz)")
set(NAM_KSPLIT 14 CACHE STRING "Dual-core layer split (14 = optimal)")
add_executable(nam_bench src/benchmark.cpp ${NAM_MODEL_SOURCE})
target_include_directories(nam_bench PRIVATE include)
target_compile_definitions(nam_bench PRIVATE TARGET_SYS_KHZ=${NAM_SYS_KHZ} KSPLIT=${NAM_KSPLIT})
target_link_libraries(nam_bench ${NAM_ENGINE_OPT} ${NAM_HW_LIBS})
pico_enable_stdio_usb(nam_bench 1)
pico_add_extra_outputs(nam_bench)
