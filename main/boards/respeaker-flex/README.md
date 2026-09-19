# ReSpeaker Flex (XIAO ESP32S3 + XVF3800)

板级支持移植自 Seeed 的 xiaozhi fork，独立成 `respeaker-flex`，不与上游
`xiao-esp32s3-sense` 冲突。

| 项目 | 取值 |
| --- | --- |
| I2S BCK / WS / DO / DI | GPIO8 / GPIO7 / GPIO44 / GPIO43 |
| 采样率（输入=输出） | 16000 Hz |
| I2C SDA / SCL | GPIO5 / GPIO6 |
| XVF3800 I2C 地址 | 0x2C |
| LED | GPIO21 |
| BOOT 按键 | GPIO0 |
| Flash | 8 MB，分区表 `partitions/v1/8m_custom_wakeword.csv` |
| 唤醒词 | MultiNet（`ni hao xiao zhi` / 你好小智） |

GPIO43/GPIO44 原为 UART0 TX/RX，被音频占用，串口日志需走 USB-Serial-JTAG。

摄像头引脚全部置 -1，未接线。OLED 非标配：启动时探测 0x3C，无响应则使用
`NoDisplay`。
