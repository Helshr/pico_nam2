#!/bin/sh
set -eu
cd "$(dirname "$0")"
SDK="$PWD/.tools/pico-sdk"
TOOLCHAIN="$PWD/.tools/arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi"
CMAKE="$PWD/.tools/cmake-3.31.8-macos-universal/CMake.app/Contents/bin/cmake"
"$CMAKE" -S . -B build-webusb -DCMAKE_BUILD_TYPE=Release -DNAM_RUNTIME_SYS_KHZ=300000 \
  -DPICO_SDK_PATH="$SDK" -DPICO_TOOLCHAIN_PATH="$TOOLCHAIN"
"$CMAKE" --build build-webusb --target pico_nam_loopback -j 8
echo "UF2: $PWD/build-webusb/pico_nam_loopback.uf2"
