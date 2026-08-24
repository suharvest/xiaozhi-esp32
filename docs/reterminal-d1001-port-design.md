# reTerminal D1001（ESP32-P4）XiaoZhi 适配设计

状态：设计稿，供实现评审使用

目标分支：`feat/reterminal-d1001`

首版重点：让 XiaoZhi 在 D1001 上稳定完成联网、双麦采音和机身扬声器播放

## 1. 结论先行

本适配沿用 XiaoZhi 现有架构，在 `main/boards/seeed-studio/reterminal-d1001/` 新增独立板级目录，不复制或改造 SenseCAP Watcher 的整套实现，也不改变任何现有板型的默认行为。

首版交付应定义为：

- ESP32-P4 运行 XiaoZhi 主程序；ESP32-C6 通过 SDIO 提供 Wi-Fi。
- 使用机身 ES8311 + NS4150B 播放 XiaoZhi 语音。
- 使用机身 ES7210 + 双麦克风采集语音。
- 提供基本屏幕状态页、触摸和实体按键入口。
- 不加入 RemoteDisplay、人脸识别、摄像头、Watcher AI、LTE、SD 卡等功能。

必须澄清：“普通蓝牙音箱”通常指手机通过 Bluetooth Classic A2DP 向音箱推流。D1001 板载 ESP32-C6 只支持 Bluetooth LE，不支持 Bluetooth Classic，因此现有硬件不能实现 A2DP Sink。ESP-Hosted 可以把 C6 的 BLE 能力提供给 P4，但不会增加 Classic/A2DP 能力。

因此首版中的“音响支持”是 **XiaoZhi 经网络收到音频后由机身扬声器播放**，不是“手机蓝牙播放音乐”。BLE 配网或 BLE 控制可另列后续里程碑；它不应阻塞首版。

## 2. 范围

### 2.1 首版必须完成

1. 新板型可由 `scripts/build.py` 独立选择并生成 ESP32-P4 固件。
2. P4 能复位并启动 C6，ESP-Hosted SDIO 链路稳定，Wi-Fi 配网和联网可用。
3. PCA9535 初始化正确，保持整机电源，并可显式控制音频功放。
4. ES8311 扬声器播放正常，支持音量调整、静音和无明显启动爆音。
5. ES7210 双麦采集正常，格式能够接入 XiaoZhi 的 AFE/唤醒词/上行音频链路。
6. 800×1280 MIPI-DSI 屏显示 XiaoZhi 基础 UI；GSL3670 触摸可用。
7. GPIO3 实体按键具备 XiaoZhi 默认的对话/配网行为。
8. 断电重启、重新配网、OTA 后能够恢复正常工作。

### 2.2 首版明确不做

- Bluetooth Classic A2DP 音箱。
- BLE Audio（包括 LE Audio/BAP/CAP）。
- RemoteDisplay。
- 人脸识别和视觉业务逻辑（板载 MIPI-CSI 摄像头本身已在后续增量中接入，见 6.、9.）。
- SenseCAP Watcher 的 SSCMA、视觉推理或事件管线迁移。
- LTE、SD 卡、RTC、IMU、电池电量和完整电源管理 UI。
- 对服务端协议、Warehouse Service 或 MCP 层做任何配套修改。

## 3. 参考基线

实现时按以下优先级取资料：

1. Seeed D1001 官方 BSP：引脚、上电时序、LCD 初始化命令和触摸驱动的事实来源。
2. XiaoZhi 上游 P4 板型：工程组织、`WifiBoard`、MIPI 显示和 ESP-Hosted 配置的结构来源。
3. XiaoZhi `BoxAudioCodec`：ES8311/ES7210 与 `esp_codec_dev` 的接口风格来源，但不能直接复用其 I2S 拓扑。

参考链接：

- [reTerminal D10xx 产品文档](https://wiki.seeedstudio.com/reterminal_d10xx_main_page/)
- [Seeed reTerminal-D1001 官方仓库](https://github.com/Seeed-Studio/reTerminal-D1001)
- [ESP32-C6 数据手册](https://documentation.espressif.com/esp32-c6_datasheet_en.html)
- [ESP-Hosted MCU：P4 Function EV Board](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/esp32_p4_function_ev_board.md)

## 4. 硬件事实表

| 模块 | 器件/接口 | D1001 配置 |
|---|---|---|
| 主控 | ESP32-P4NRW32 | 32 MB PSRAM，产品资料标注 32 MB QSPI Flash |
| 无线协处理器 | ESP32-C6 | Wi-Fi 6 + Bluetooth LE，经 SDIO 连接 P4 |
| 音频控制 I2C | I2C1 | SDA GPIO20，SCL GPIO21 |
| IO 扩展 | PCA9535 | 地址 `0x20` |
| 播放 Codec | ES8311 | 地址 `0x18` |
| 播放 I2S | I2S TX | MCLK 33，BCLK 32，WS 31，DOUT 30 |
| 功放 | NS4150B | PCA9535 P13，即官方 BSP 的 `EXP_GPO11`/bit 11，高电平开启 |
| 录音 ADC | ES7210 | 地址 `0x40` |
| 录音 I2S | I2S RX/TDM | MCLK 29，BCLK 28，LRCK 27，DIN 26 |
| 屏幕 | JD9365DA-H3 | 800×1280，MIPI-DSI 2 lane，PHY LDO3 2.5 V |
| 触摸 | GSL3670 | I2C0：SDA 37，SCL 38；INT GPIO16 |
| 主按键 | GPIO | GPIO3 |
| C6 SDIO | P4 ↔ C6 | CMD 6，D0 7，D1 8，D2 9，D3 10，CLK 11，RESET/CHIP_PU 13 |

注意：Seeed 工厂工程中出现过 16 MB 的 sdkconfig 默认值，但产品硬件资料标注 32 MB Flash。实现不得盲抄工厂 sdkconfig；在量产固件确定分区表前，应读取真实 flash ID/容量验证。板型配置暂按 32 MB 设计，验证失败时停止烧写而不是自动降级。

## 5. 软件结构

建议新增：

```text
main/boards/seeed-studio/reterminal-d1001/
├── config.json
├── config.h
├── reterminal-d1001.cc
├── reterminal_d1001_audio_codec.h
├── reterminal_d1001_audio_codec.cc
├── reterminal_d1001_expander.h
├── reterminal_d1001_expander.cc
├── lcd_init_cmds.h
└── README.md
```

同时只做必要的全局注册：

- `main/Kconfig.projbuild`：增加 `CONFIG_BOARD_TYPE_RETERMINAL_D1001`。
- `main/CMakeLists.txt`：把该配置映射到 `seeed-studio/reterminal-d1001`，选择适合 800×1280 的字体资源。
- `main/idf_component.yml`：补充 GSL3670/PCA9535 所需组件。JD9365、ESP-Hosted、ESP Wi-Fi Remote 和音频组件目前已有，不重复引入。

`config.json` 建议保持一个首版 variant：

- `manufacturer`: `seeed-studio`
- `type`: `reterminal-d1001`
- `name`: `reterminal-d1001`
- `target`: `esp32p4`
- 使用 32 MB v2 分区表。
- 打开 PSRAM、ESP32-C6 slave target、ESP-Hosted SDIO 4-bit 和 P4 相关修订配置。
- 关闭 RemoteDisplay 和 Watcher 专属组件；板载摄像头在后续增量中打开（`CONFIG_CAMERA_SC202CS`）。

## 6. 初始化顺序

板类继承 `WifiBoard`。构造阶段按以下顺序执行，任何关键步骤失败都打印明确错误并停止依赖它的后续初始化：

1. 初始化 I2C1（GPIO20/21）。
2. 初始化 PCA9535。
3. 拉起 power-hold；功放默认保持关闭。
4. 初始化 C6 reset/ESP-Hosted 所需板级配置。
5. 初始化音频 Codec 和两组 I2S。
6. 向 TX DMA 预装静音帧，再打开 ES8311，最后拉高功放使能，避免爆音。
7. 初始化 MIPI DSI PHY、显示供电、LCD reset 和背光。
8. 初始化 I2C0 与 GSL3670 触摸。
9. 走 BSP 摄像头上电时序（CAM_EN → CAM_PWDN/CAM_RST → reset 脉冲），在 I2C0 上探测 SCCB `0x36`，探到再创建 `EspVideo`；探不到就断电跳过，`GetCamera()` 返回 `nullptr`。
10. 初始化 GPIO3 按键并进入 XiaoZhi 正常启动流程。

不要直接调用 Seeed 工厂 BSP 的完整 `bsp_power_init()`：它会同时初始化 LTE、电池、RTC、SD 等不需要的外设，扩大失败面和功耗。只移植首版所需的最小上电序列。

## 7. 音频设计

### 7.1 为什么不能直接使用 `BoxAudioCodec`

现有 `BoxAudioCodec` 假定 ES8311 和 ES7210 共用同一个 MCLK/BCLK/WS，并在 I2S0 上创建一对 TX/RX channel。D1001 的硬件不是这个拓扑：

- ES8311 使用 GPIO33/32/31/30 的独立播放总线。
- ES7210 使用 GPIO29/28/27/26 的独立录音总线。
- Seeed BSP 使用 I2S TX port 0 和 I2S RX port 1。

因此应新建 `ReTerminalD1001AudioCodec`，沿用 `AudioCodec` 和 `esp_codec_dev` 接口，但分别创建 TX 与 RX channel。不要为了适配 D1001 修改 `BoxAudioCodec` 的既有行为，以免影响其他板型。

### 7.2 播放路径

- I2S0 TX，标准 I2S，16-bit mono，首选 24 kHz 与 XiaoZhi 当前音频链路保持一致。
- MCLK multiple 256。
- ES8311 只配置 DAC 模式。
- `pa_pin` 必须按工厂代码填 GPIO53（`pa_reverted=false`）：NS4150B 的使能脚由 ES8311 驱动在设备开/关时拉动；PCA9535 bit 11 只是功放电源轨。实机验证：pa_pin 置 NC 时 DAC 信号在 TDM 回采槽清晰可见但扬声器完全无声。
- 开启输出：准备 DMA 静音数据 → open codec → 设置音量 → 打开 PA。
- 关闭输出：先关闭 PA → close codec，避免尾音和爆音。

### 7.3 录音路径

- I2S1 RX，TDM 4 slot，16-bit，首选 24 kHz；若 ES7210 或 AFE 在实机上不稳定，再以 16 kHz 作为诊断基线。
- ES7210 可选择 4 个物理 mic 输入，但产品只有双麦。首轮通过录制原始四槽数据确认实际有效槽位，再固定 `channel_mask`，不能凭参考板猜测。
- XiaoZhi AFE 需要一个近端麦克风通道；设备 AEC 若需要参考通道，应优先使用软件播放参考，不应把另一个物理麦槽未经验证地当作回声参考。
- 首版先保证全双工不死锁、采集无错位，再开启 `CONFIG_USE_DEVICE_AEC` 并进行回声测试。

### 7.4 音频验收

- 1 kHz 测试音连续播放 10 分钟，无 DMA underrun、破音和异常重启。
- 0%、30%、70%、100% 音量可辨且静音有效。
- 冷启动、首次播音和停止播放无明显爆音。
- 分别对两个实体麦克风近讲，确认采样槽位、幅度和左右一致性。
- 连续双向对话 30 分钟，无 I2S timeout、AEC 发散或堆内存持续下降。

## 8. 无线与 C6 固件

P4 本身没有 Wi-Fi/Bluetooth，必须依赖 C6。首版先把 Wi-Fi 作为门槛，BLE 不进入关键路径。

实现步骤：

1. 复用上游 P4 板型的 `esp_hosted` 和 `esp_wifi_remote` 版本，不自行建立另一套网络栈。
2. 在 `config.json` 中写入 D1001 的 SDIO 引脚和 ESP32-C6 target 配置。
3. 启动时记录 P4 端组件版本与 C6 slave firmware 版本。
4. 先验证出厂 C6 固件是否与当前 ESP-Hosted 主机组件兼容。
5. 若不兼容，单独产出并记录 C6 固件、烧录步骤和版本矩阵；不得在普通 P4 OTA 中静默覆盖 C6。

验收包括冷启动 20 次、Wi-Fi 重连、热点切换、持续对话 30 分钟。C6 固件版本不匹配时应给出可诊断日志，不能表现为无限“正在连接”。

## 9. 显示、触摸和按键

- 复用 XiaoZhi `MipiLcdDisplay` 架构与上游 P4/JD9365 实现。
- LCD 初始化命令、DPI timing、reset 和供电顺序以 Seeed D1001 BSP 为准，不使用其他 8 英寸 JD9365 面板的参数替代。
- MIPI DSI：2 lane；PHY LDO channel 3，2500 mV。
- LCD、背光、touch reset 中由 PCA9535 控制的信号统一经过 expander 封装，不在板类中散落 magic bit。
- 首版 UI 只需要现有 XiaoZhi 状态、文字和基础表情，不新增 RemoteDisplay 页面。
- GPIO3 延续现有 XiaoZhi 交互：启动阶段进入配网，运行阶段切换对话状态。

## 10. 向后兼容与隔离原则

- 新功能仅在 `CONFIG_BOARD_TYPE_RETERMINAL_D1001` 下编译。
- 不修改现有 `BoxAudioCodec`、P4 参考板和 SenseCAP Watcher 的默认配置。
- 新增依赖必须使用 target/board 条件，不能增加 S3/C3 等固件体积或改变其依赖解析。
- `type` 和 `name` 一经首版发布即视为 OTA 兼容标识，不随营销名称更改。
- 本分支基于最新上游 P4 支持开发；不要把本地 `main` 整体 merge 进来。确需使用本地安全修复时按独立 commit 逐个挑选，并单独回归。
- C6 固件升级与 P4 应用 OTA 分离，避免已有设备仅升级主固件后失联。

## 11. 实施拆分

建议按以下顺序交付小提交，每个提交均可独立审查：

1. **板型骨架**：Kconfig、CMake、config.json、config.h，可完成配置和空板编译。
2. **最小电源与 expander**：I2C1、PCA9535、power-hold、PA 控制及单元化封装。
3. **C6/Wi-Fi**：ESP-Hosted SDIO、版本日志、联网验证。
4. **扬声器**：独立 I2S0 TX、ES8311、功放时序和音量。
5. **麦克风**：独立 I2S1 RX/TDM、ES7210、槽位验证和 XiaoZhi AFE 接入。
6. **显示**：MIPI DSI、JD9365、基础 UI 和背光。
7. **触摸与按键**：GSL3670、GPIO3 行为。
8. **整机稳定性**：AEC、重连、功耗、冷启动和长稳测试。

若首轮只想最快验证“音响”，第 1～5 项构成最小可用路径，显示可临时使用 `NoDisplay` 做开发诊断；正式首版仍应完成第 6～8 项。

## 12. 完成定义

同时满足以下条件才能认为首版适配完成：

- `scripts/build.py reterminal-d1001` 可在干净环境完成配置、编译和打包。
- 不修改其他板型的生成配置，至少完成一款现有 P4 板的回归编译。
- 实机完成配网、连接既有 XiaoZhi 服务、唤醒/按键对话、双麦上行和扬声器下行。
- 屏幕、触摸和按键工作，摄像头/RemoteDisplay/Watcher 功能没有被误启用。
- 连续运行 30 分钟并完成 20 次冷启动，无崩溃、明显内存泄漏或 C6 链路失联。
- 文档记录 P4 固件版本、C6 固件版本、ESP-IDF/ESP-Hosted 版本和烧录方式。

## 13. 开发与烧录注意事项

- ESP32-P4 与原项目常用的 ESP32-S3 target 不同，不复用已有 S3 的 `build/` 缓存。建议使用独立 worktree/checkout，让该目录的 `build/` 专用于 D1001。
- 不要为了“修编译”反复 `fullclean` 后直接烧录；先保存完整构建日志并确认 target、分区表和 flash 容量。
- 首次烧写前备份出厂固件和分区表，尤其是 C6 slave firmware。
- P4 与 C6 是两份固件、两个版本域，所有测试记录必须分别标注，避免把 C6 兼容问题误判为 XiaoZhi 应用问题。
