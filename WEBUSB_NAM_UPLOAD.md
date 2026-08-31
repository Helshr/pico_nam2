# Pico NAM WebHID 固件

## 当前固件

- 构建目录：`build-webhid/`
- UF2：`build-webhid/pico_nam_loopback.uf2`
- 当前内置模型：`example.nam`（构建时转换为 A2-Lite）
- WebHID VID：`0xCAFE`
- HID report：64 bytes

## 构建

```sh
cd ~/Products/pico-neural-amp-modeler-demo
.tools/cmake-3.31.8-macos-universal/CMake.app/Contents/bin/cmake -S . -B build-webusb \
  -DCMAKE_BUILD_TYPE=Release -DNAM_RUNTIME_SYS_KHZ=300000 \
  -DPICO_SDK_PATH="$PWD/.tools/pico-sdk" \
  -DPICO_TOOLCHAIN_PATH="$PWD/.tools/arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi"
.tools/cmake-3.31.8-macos-universal/CMake.app/Contents/bin/cmake \
  --build build-webusb --target pico_nam_loopback -j 8
```

## 烧录

1. 按住 Pico 的 BOOTSEL，再插拔 USB，直到出现 `/Volumes/RP2350`。
2. 执行：

```sh
cp -X build-webusb/pico_nam_loopback.uf2 /Volumes/RP2350/
sync
```

烧录后设备会重新枚举为复合 USB 音频 + WebUSB vendor 设备。NAM 默认旁路，短按 BOOTSEL 可切换音色处理。

## 上传协议

网页读取 `.nam` JSON，选择 A2-Lite 子模型，然后通过 WebHID 发送：

| 偏移 | 类型 | 内容 |
|---|---|---|
| 0 | 4 bytes | ASCII `NAMW` |
| 4 | uint32 LE | 版本 `1` |
| 8 | uint32 LE | float32 权重数量 |
| 12 | uint32 LE | 后续 payload 字节数 |
| 16 | bytes | float32 权重数组（little-endian） |

固件在 RAM 中重建双核 A2-Lite 模型并回复 `OK:MODEL` 或 `ERR:*`。当前上传模型只在本次上电期间有效，重启后恢复构建时的 `example.nam`。
