# WM8978 NAM 诊断记录

当前 blockqueue 固件通过 USB CDC 提供以下控制命令：

```text
STATUS       # 返回 NAM 或 BYPASS
NAM          # 开启 NAM
BYPASS       # 旁路
LEVEL        # 返回最近 ADC 块峰值、非零样本数和队列故障标记
DIAG         # 输出固定方波，隔离 DAC/I2S 输出链路
NAMDIG       # 用内部 440 Hz 正弦实际运行 NAM，隔离模拟输入
BOOTLOADER   # 进入 BOOTSEL，供脚本烧录
```

`LEVEL` 输出格式为 `L=peak/rms R=peak/rms A=active N=nonzero O=overload F=fault U=tx_underrun`；峰值和 RMS
是 WM8978 ADC 左/右 I2S 槽的原始 16-bit 数据，不经过 NAM。`F=0` 表示固定块队列没有丢失所有权，`U`
只统计 TX 端明确补出的静音块。

回环脚本 `test_ixo12_wm8978_loop.py` 现在会在播放期间自动轮询 Pico 的 `LEVEL`，并在启动时
提示关闭 IXO12 的 MONITOR/Loopback。这样可以直接区分两类问题：播放时 `ADC_PEAK` 从空闲的
约 20--40 上升，才说明 DI 已到达 WM8978；始终保持空闲值则应先检查模拟线材、插孔和 IXO12
输出路由，不应继续调整 NAM 参数。

脚本会在整个测试期间保持一个 CDC 连接，不会每次轮询都重新打开串口；这避免 macOS 在
反复打开/关闭 `/dev/cu.usbmodem*` 时触发 USB 重枚举。若只想录音而不轮询，可加
`--no-level-poll`。

已验证：`DIAG` 回录输出干净，说明 DAC 发送端和耳机输出链路基本正常。`NAMDIG` 会调用真实
`nam_fx_process()`，重启或重新烧录后恢复正常 NAM。当前输入寄存器沿用已保存的 WM8978
旁路/NAM 基线：R44=`0x000`（关闭未连接的麦克风 PGA），R45/R46=`0x040/0x140`，
R47/R48=`0x140`（L2/R2 单端 LINE IN，0 dB）。

若播放 DI 时 `LEVEL` 只有几十（例如 `L=30/10`），信号没有进入 ADC，应检查 LINE IN 线和 IXO12 direct-monitor。
若 `L` 或 `R` 峰值接近 32767 且 RMS 也很高，则输入削波或形成反馈，应先降低 IXO12 增益。

最近一次低电平 1 kHz 探针在 `LEVEL` 中仍为 0，且 IXO12 回录峰值达到 0 dBFS；这不是 NAM
失真证据，而是声卡直通/回路或当前模拟线没有接到 WM8978 ADC。重启同一版固件后空闲
`ADC_PEAK 20`、`RMS 8`、`ACTIVE 0`、`QUEUE_FAULT 0`，数字传输层本身已恢复运行。

随后执行 `NAMDIG` 的纯数字自检，IXO12 回录得到约 `mean -40.7 dB / peak -33.3 dB` 的稳定
双声道信号；自检结束后已重新烧录并恢复到 `NAM`。这证明 NAM 引擎、双核处理、I2S DAC 和
WM8978 耳机输出链路可以工作，剩余未闭环的是外部 LINE IN 到 WM8978 ADC 的物理路径。

本轮再次执行 `NAMDIG`，IXO12 回录 3 秒得到 `mean -40.9 dB / peak -33.0 dB`，随后已发送
`NAM` 恢复默认模式；数字输出自检仍稳定。

最新固件增加了 `C=rx_done/tx_done/rx_read/tx_write` 计数。`NAMDIG` 连续 5 秒回录为
`mean -40.6 dB / peak -32.4 dB`，且计数持续递增；退出诊断后 `NAM` 的计数也持续递增，
因此此前“诊断播放一小段后停住”是旧状态残留，当前模式切换已清理诊断标志。

正式固件重新烧录后，空闲 `LEVEL` 的大多数块为 `L=60--120/20--60`，但偶尔出现无播放时的
`L=3448/3396 R=1744/1716`、`L=2262/2231 R=3794/3754` 等成块突发，`A=48`。这说明当前
WM8978 输入端/模拟线存在间歇噪声或悬空，而不是播放文件或 NAM 产生的噪声；后续应在物理输入
断开、接地和 IXO12 输出静音的条件下重复 `LEVEL`，再决定是否需要修改模拟路由。

再用 Mac `External Headphones` 播放参考 WAV、增益 `0.01` 做 30 秒测试，ADC 仍持续出现
`L=3k--12k/… R=1k--11k/… A=48` 的间歇成块突发，且偶尔左侧达到满幅；回录整体
`mean -48.0 dB / peak -18.2 dB`，但每 100 ms 区间呈现“有突发/静音”交替，右回录通道始终
约 `-90 dBFS`。这与参考音频的连续区间完全不一致，进一步排除 NAM 音色作为根因，指向
模拟输入噪声、线材/插孔或声卡监控路由；正式固件仍已恢复为 `NAM`。

IXO12 的前面板 `MONITOR` 键同时控制 loopback/direct monitoring。回环自测时应关闭它，
否则 IXO12 会把电脑播放信号和输入再次混回输出，形成无限反馈；Steinberg 官方手册也明确
警告这种配置会产生 feedback。参见 [IXO22/IXO12 Operation Manual](https://download.steinberg.net/downloads_hardware/IXO/Manuals/IXO22_IXO12_Operation_Manual/IXO22_IXO12_Operation_Manual_English.pdf)，第 5、13 页。
最近一次使用参考 DI（播放增益 `0.01`）的 12 秒测试中，`LEVEL` 全程约 `L=18--43/7--15`、
`R=17--44/7--15`、`A=0`、`F=1`、`U=1`，回录峰值 `-84.3 dBFS`、平均 `-91.0 dBFS`；与参考音频
每 100 ms 区间（约 `-16--33 dBFS`）不匹配。这确认当前外部模拟输入仍未送入 WM8978 ADC。`U=1`
是上电 NAM 预热期间的一次欠载，之后没有继续增长；它不是这次静音的原因。测试结束后已发送
`NAM`，板子保持 NAM 模式。

随后按实际接线将播放端改为 Mac `External Headphones`（而不是 IXO12 输出）再次测试 10 秒，
结果仍为 `L=20--43/8--15`、`A=0`、回录 `-84.3/-91.0 dBFS`。因此无论 DI 从 IXO12 输出还是
Mac 耳机输出发送，WM8978 ADC 都没有收到稳定的连续音频；下一步应只检查模拟线是否插在 WM8978
`音频输入` 插孔、IXO12 的物理输出口/电平和地线，软件侧不要再改 NAM 参数。

本轮重新将正式固件的寄存器路由对齐到旧成功路径后，参考音频回环的 `LEVEL` 仍表现为“低电平底噪与成块突发交替”（例如
`L=13935/5840`、`L=32704/8523`），而不是随参考音频连续变化；回录左声道约 `-40.9 dBFS`，右声道约 `-90 dBFS`。
比较脚本现已支持 24-bit 参考 WAV，可直接输出每 100 ms 的完整区间指标。该结果进一步说明剩余问题在外部模拟输入/声卡监控路径，不能通过继续调 NAM 增益解决。

为隔离“传输层”和 NAM 推理，本轮临时烧录了已保存的 `WM8978_LINE_BYPASS=ON` 连续 DMA 镜像
（`build-pico-wm8978-line-bypass/pico_wm8978_nam.uf2`）。使用同一参考 WAV、Mac `External Headphones`
输出和 IXO12 输入回录 10 秒，得到左声道 `mean=-47.1 dBFS / peak=-17.6 dBFS`，说明旧连续 DMA
确实能把模拟输入送到回录端；但它是旁路镜像、没有 USB CDC，不能作为最终 NAM 固件。由于该镜像
没有 USB 接口，测试后无法由电脑自动触发 BOOTSEL；下一次板子进入实体 BOOTSEL 后，必须优先恢复
`build-pico-wm8978-blockqueue/pico_wm8978_nam_blockqueue.uf2`，再继续 NAM 对照，避免把旁路结果
误认为 NAM 已通过。
