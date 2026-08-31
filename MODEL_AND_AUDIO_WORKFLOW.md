# Pico NAM 模型与音频工作流程

项目目录：

```text
/Users/Henry/Products/pico-neural-amp-modeler-demo
```

## 一、替换 NAM 模型并刷入板子

板子一次只内置一个 NAM。`.nam` 文件会在编译时转换成 C 数据并嵌入固件，运行时不从电脑读取。

### 1. 备份当前模型

```bash
cd ~/Products/pico-neural-amp-modeler-demo
cp -p example.nam example.nam.backup
```

### 2. 替换模型

例如使用 Downloads 中的模型：

```bash
cp -p \
  "$HOME/Downloads/Friedman BE100 Deluxe (EL34) community pack/[AMP] BE100DLX-BE Eddie #01 - BLEND.nam" \
  example.nam
```

### 3. 编译

```bash
.tools/cmake-3.31.8-macos-universal/CMake.app/Contents/bin/cmake \
  -S . -B build-model -DCMAKE_BUILD_TYPE=Release \
  -DNAM_RUNTIME_SYS_KHZ=300000 \
  -DPICO_SDK_PATH="$PWD/.tools/pico-sdk" \
  -DPICO_TOOLCHAIN_PATH="$PWD/.tools/arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi"

.tools/cmake-3.31.8-macos-universal/CMake.app/Contents/bin/cmake \
  --build build-model --target pico_nam_loopback -j 8
```

生成的固件：

```text
build-model/pico_nam_loopback.uf2
```

### 4. 刷写

1. 按住板子的 `BOOTSEL`，插入 USB。
2. Finder 中出现 `RP2350` 磁盘后执行：

```bash
cp -X build-model/pico_nam_loopback.uf2 /Volumes/RP2350/
sync
```

3. 板子会自动重启；macOS 应重新看到 `Pico NAM` 音频设备。

### 5. 启用/旁路 NAM

板子正常运行后短按一次 `BOOTSEL`：

- LED 亮：NAM 开启；
- LED 灭：旁路，声音原样通过。

不要在插 USB 时长按以外反复按住 `BOOTSEL`，长按会进入刷写模式。

## 二、替换待处理的音频

音频不会写入板子；它只是由电脑播放，通过 USB 发送给板子，板子返回处理后的音频。

### 在线监听

推荐使用 GarageBand 监听返回音频：

- GarageBand 输入：`Pico NAM`；
- GarageBand 输出：`MacBook Pro Speakers`；
- 打开音轨的 `Monitoring`；
- 系统输出设为 `Pico NAM`；
- 用 QuickTime 或“音乐”播放待处理文件。

不要在 GarageBand 内直接播放待处理文件，否则它会绕过 Pico NAM。

### 离线处理

项目中提供了测试脚本：

```bash
cd ~/Products/pico-neural-amp-modeler-demo
./test_pico_nam_offline.py
```

它会生成测试音、送入 Pico NAM、录下返回音频并播放结果。

处理自己的文件时，建议使用 WAV、单声道、干净的吉他 DI 音频。吉他功放 NAM 不适合直接处理完整混音或已经失真的成品音乐。

## 三、验证设备

```bash
SwitchAudioSource -c -t output
SwitchAudioSource -c -t input
system_profiler SPAudioDataType
```

正常时应看到 `Pico NAM`，并且是 2 入 / 2 出、48 kHz。
