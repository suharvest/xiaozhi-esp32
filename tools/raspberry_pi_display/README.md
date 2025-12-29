# Xiaozhi Remote Display Server

将 SenseCAP Watcher 的 UI 投屏到树莓派或电脑上显示。

## 工作原理

采用 **UI 状态同步** 方式：
- ESP32 设备发送 UI 状态（表情、状态文字、聊天消息等）作为 JSON 数据
- 树莓派/电脑端使用 Pygame 根据状态重新渲染 UI
- 同时转发音频流（Opus 编码）

相比截图方案，这种方式：
- 数据量小（100-500 字节 vs 10-50KB）
- 对设备性能压力小
- 延迟更低

## 安装

### 依赖

```bash
# 使用 uv (推荐)
cd tools/raspberry_pi_display
uv sync

# 或使用 pip
pip install -r requirements.txt
```

### 树莓派额外依赖

```bash
# 安装中文字体
sudo apt install fonts-wqy-zenhei fonts-wqy-microhei

# 安装音频依赖 (可选，用于播放音频)
sudo apt install portaudio19-dev libopus-dev
pip install pyaudio opuslib
```

## 使用方法

### 1. 启动服务器

```bash
cd tools/raspberry_pi_display
uv run python server.py
```

### 2. 配置 ESP32

编辑 `main/boards/sensecap-watcher/config.h`：

```cpp
#define REMOTE_DISPLAY_ENABLED      true
#define REMOTE_DISPLAY_SERVER_URL   "ws://你的IP地址:8765"
```

然后重新编译烧录固件。

### 3. 连接

设备启动后会自动连接到服务器，连接成功后 UI 会同步显示。

## 快捷键

| 按键 | 功能 |
|------|------|
| `F` / `F11` | 切换全屏/窗口模式 |
| `q` / `ESC` | 退出 |

## 环境变量配置

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `RD_HOST` | `0.0.0.0` | 监听地址 |
| `RD_PORT` | `8765` | 监听端口 |
| `RD_SCALE` | `1.5` | 窗口缩放比例 |
| `RD_FULLSCREEN` | `0` | 启动时全屏 (`1` 启用) |

示例：
```bash
RD_FULLSCREEN=1 RD_SCALE=2.0 uv run python server.py
```

## 文件结构

```
raspberry_pi_display/
├── server.py        # WebSocket 服务器主程序
├── ui_renderer.py   # Pygame UI 渲染器
├── audio_player.py  # Opus 音频解码播放
├── config.py        # 配置管理
├── assets/          # UI 资源文件 (表情、背景图)
│   ├── emojis/      # 表情图片 (PNG)
│   └── backgrounds/ # 背景图片 (PNG)
└── requirements.txt
```

## 自定义资源

将表情图片放入 `assets/emojis/` 目录，文件名对应表情名称：
- `neutral.png` - 中性表情
- `happy.png` - 开心
- `sad.png` - 悲伤
- 等等...

背景图片放入 `assets/backgrounds/` 目录。

如果没有对应的资源文件，会使用程序绘制的简易表情作为替代。
