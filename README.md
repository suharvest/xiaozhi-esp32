# Reachy Mini <-> Xiaozhi LLM Conversation Bridge

将 [Reachy Mini](https://github.com/pollen-robotics/reachy_mini) 机器人接入 [Xiaozhi (小智)](https://github.com/78/xiaozhi-esp32) 对话服务器，实现基于 LLM 的语音对话 + 机器人表情动作。

## 架构概述

```
┌─────────────────────┐       WebSocket        ┌──────────────────────┐
│    Reachy Mini      │    (OPUS audio + JSON)  │   Xiaozhi Server     │
│                     │ ◄─────────────────────► │                      │
│  🎤 Microphone      │                         │  ASR (语音识别)       │
│  🔊 Speaker         │   Python Bridge         │  LLM (大语言模型)     │
│  🤖 Head/Antennas   │   (本项目)              │  TTS (语音合成)       │
│                     │                         │  MCP (工具调用)       │
└─────────────────────┘                         └──────────────────────┘
```

### 数据流

1. **语音输入**: Reachy Mini 麦克风 -> PCM 采集 -> OPUS 编码 -> WebSocket -> Xiaozhi Server
2. **语音识别**: Xiaozhi Server ASR 识别用户语音 -> 发送 STT 文本
3. **LLM 推理**: Xiaozhi Server LLM 生成回复 + 情绪标签
4. **语音输出**: Xiaozhi Server TTS -> OPUS 音频 -> WebSocket -> OPUS 解码 -> Reachy Mini 扬声器
5. **表情动作**: 情绪标签 -> EmotionMapper -> Reachy Mini 头部/天线动作

## 前置要求

- Reachy Mini 机器人（或 Reachy Mini Lite USB 版本）
- Python 3.10+
- 运行中的 Xiaozhi 对话服务器
- OPUS 编解码库 (`libopus-dev`)

## 安装

```bash
# 安装系统依赖 (Ubuntu/Debian)
sudo apt install libopus-dev

# 安装 Python 包
cd reachy_mini_bridge
pip install -e .
```

## 配置

```bash
# 生成默认配置文件
reachy-xiaozhi --generate-config

# 或者从示例复制
cp bridge_config.example.yaml bridge_config.yaml
```

编辑 `bridge_config.yaml`，主要需要修改：

- `server.websocket_url`: 你的 Xiaozhi 服务器地址
- `server.token`: 认证令牌（如果需要）
- `audio.media_backend`: 根据你的 Reachy Mini 版本选择
  - `default_no_video`: USB 连接的 Lite 版本
  - `webrtc`: 无线版本

## 运行

```bash
# 使用配置文件运行
reachy-xiaozhi -c bridge_config.yaml

# 或直接指定服务器地址
reachy-xiaozhi -u ws://your-server:9005/xiaozhi/v1/ -t your-token

# 开启调试日志
reachy-xiaozhi -v
```

## 工作原理

### 1. Xiaozhi 协议兼容

本桥接器实现了与 xiaozhi-esp32 设备相同的 WebSocket 协议：

- **Hello 握手**: 发送设备信息和音频参数，接收 session_id
- **音频流**: 双向 OPUS 编码音频流（二进制帧）
- **控制消息**: JSON 格式的 TTS/STT/LLM/MCP 消息

因此 Xiaozhi 服务器会将 Reachy Mini 当作一个标准的 xiaozhi 客户端设备对待。

### 2. 音频管道

```
Reachy Mini Mic -> float32 PCM -> 重采样到 16kHz -> OPUS 编码 -> WebSocket 发送
                                                                        |
WebSocket 接收 <- OPUS 解码 <- 重采样到输出采样率 <- float32 PCM <- Reachy Mini Speaker
```

### 3. 情绪表达映射

服务器 LLM 产生的情绪标签会被转换为机器人动作：

| 情绪 | 动作 |
|------|------|
| happy/开心 | 点头 + 天线上扬 |
| thinking/思考 | 头侧倾 + 天线不对称 |
| sad/伤心 | 低头 + 天线下垂 |
| surprised/惊讶 | 头后仰 + 天线高举 |
| curious/好奇 | 微微前倾侧头 |
| neutral/平静 | 回到中心位置 |

### 4. 项目结构

```
reachy_mini_bridge/
├── pyproject.toml                      # Python 包配置
├── bridge_config.example.yaml          # 示例配置
├── README.md                           # 本文档
└── reachy_mini_bridge/
    ├── __init__.py
    ├── main.py                         # 主入口 & 对话循环
    ├── config.py                       # 配置管理
    ├── xiaozhi_protocol.py             # Xiaozhi WebSocket 协议客户端
    ├── audio_bridge.py                 # 音频采集/播放 + OPUS 编解码
    ├── emotion_mapper.py               # 情绪 -> 机器人动作映射
    └── robot_controller.py             # Reachy Mini SDK 封装
```

## 与原始 xiaozhi-esp32 的对比

| 特性 | xiaozhi-esp32 (ESP32) | 本桥接器 (Reachy Mini) |
|------|----------------------|----------------------|
| 硬件 | ESP32-S3 开发板 | Reachy Mini 机器人 |
| 语言 | C++ (ESP-IDF) | Python |
| 音频 I/O | I2S 音频芯片 | Reachy Mini 麦克风/扬声器 |
| 显示 | LCD/OLED 屏幕 | 无（用头部动作代替） |
| 表情 | 屏幕表情图标 | 头部姿态 + 天线动作 |
| 唤醒词 | 本地检测 (ESP-SR) | 服务器端 / 天线按压 |
| 协议 | WebSocket / MQTT+UDP | WebSocket |

## 扩展开发

### 添加新情绪映射

编辑 `emotion_mapper.py` 中的 `EMOTION_MAP` 字典，添加新的表情定义。

### 添加 MCP 工具

可以扩展协议客户端处理 MCP 消息，让 LLM 通过工具调用控制机器人的更多功能（如拍照、跟踪等）。

### 使用 Reachy Mini REST API

对于更复杂的动作控制，可以直接调用 Reachy Mini 的 REST API (`http://localhost:8000/api/`)。
