# Xiaozhi Remote Display Server

将 SenseCAP Watcher 的 UI 投屏到树莓派或电脑上显示。

## 工作原理

采用 **UI 状态同步 + Web 渲染** 方式：
- ESP32 设备发送 UI 状态（表情、状态文字、聊天消息等）作为 JSON 数据
- 服务器转发状态到浏览器，浏览器渲染 UI
- 音频由服务器端解码播放（Opus → PCM）

优点：
- **依赖少**：无需安装 SDL2/pygame 等图形库
- **跨平台**：任何有浏览器的设备都能查看
- **多客户端**：多个浏览器可同时查看

## 安装

### 1. 系统依赖（树莓派/Linux）

```bash
# 音频库（必须）
sudo apt install portaudio19-dev libopus-dev

# PipeWire ALSA 支持（如果 HDMI 音频不工作）
sudo apt install pipewire-alsa wireplumber
```

### 2. HDMI 音频配置（reComputer/树莓派）

如果 HDMI 没有声音，按以下步骤配置：

```bash
# 1. 重启音频服务
systemctl --user restart pipewire pipewire-pulse wireplumber

# 2. 查看可用音频输出
pactl list sinks short

# 3. 设置 HDMI 为默认输出（名字根据你的设备可能不同）
pactl set-default-sink alsa_output.platform-fef00700.hdmi.hdmi-stereo

# 4. 测试
paplay /usr/share/sounds/alsa/Front_Center.wav
```

**开机自动设置 HDMI 输出**（可选）：

```bash
# 添加到 ~/.config/pipewire/pipewire.conf.d/default-sink.conf
mkdir -p ~/.config/pipewire/pipewire.conf.d
echo 'context.properties = { default.audio.sink = "alsa_output.platform-fef00700.hdmi.hdmi-stereo" }' > ~/.config/pipewire/pipewire.conf.d/default-sink.conf
```

**禁用音频**（如果不需要）：

```bash
RD_NO_AUDIO=1 uv run python server.py
```

### 3. Python 依赖

```bash
cd tools/raspberry_pi_display

# 使用 uv (推荐)
uv sync

# 或使用 pip
pip install aiohttp opuslib pyaudio
```

## 使用方法

### 1. 启动服务器

```bash
cd tools/raspberry_pi_display
uv run python server.py
```

### 2. 打开浏览器

访问 `http://localhost:8765` 或 `http://树莓派IP:8765`

### 3. 配置 ESP32

编辑 `main/boards/sensecap-watcher/config.h`：

```cpp
#define REMOTE_DISPLAY_ENABLED      true
#define REMOTE_DISPLAY_SERVER_URL   "ws://你的IP地址:8765"
```

然后重新编译烧录固件。

### 4. 连接

设备启动后会自动连接到服务器，浏览器会实时显示 UI 状态。

## 快捷键（浏览器中）

| 按键 | 功能 |
|------|------|
| `F` / `F11` | 切换全屏 |
| `q` / `ESC` | 退出全屏 |

## 环境变量配置

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `RD_HOST` | `0.0.0.0` | 监听地址 |
| `RD_PORT` | `8765` | 监听端口 |
| `RD_LOCAL_AUDIO` | `0` | 设为 `1` 启用服务端音频直出（PyAudio → HDMI）|
| `RD_DATA_DIR` | `./data` | 持久化数据目录（存放 narrate_config.json）|
| `RD_DEVICE_NAME` | `Xiaozhi Display` | mDNS 和 Web UI 中显示的设备名 |
| `RD_SCALE` | `1.5` | 显示缩放倍数 |
| `RD_NO_AUDIO` | `0` | 设为 `1` 完全禁用音频 |

示例：
```bash
RD_PORT=9000 uv run python server.py
RD_LOCAL_AUDIO=1 uv run python server.py  # HDMI 直出音频
RD_NO_AUDIO=1 uv run python server.py     # 无音频模式
```

## 音频架构

系统有**两条独立的音频通路**，按使用场景选择：

```
ESP32 音频 (Opus/PCM)
    → server.py 解码
    ├── 通路 A: PyAudio → PulseAudio → HDMI/Speaker    (RD_LOCAL_AUDIO=1)
    └── 通路 B: WebSocket → 浏览器 Web Audio API        (默认)
```

| 场景 | RD_LOCAL_AUDIO | 浏览器点击 | 声音来源 |
|------|---------------|-----------|---------|
| **reComputer 本机 kiosk** | `1` | 不点击 | HDMI 直出，一份声音 |
| **局域网远程电脑** | `0` | 点击开始 | 远程电脑扬声器 |
| **本机 + 浏览器都开** | `1` | 点击了 | ⚠️ 两份声音叠加 |

> **设计说明**：浏览器的 "Click to start" 遮罩不是 bug，而是 autoplay policy 安全机制。
> 在 kiosk 场景下它充当安全阀 — 不点击浏览器就不出声，避免与服务端音频重复。
> 如需在 kiosk 模式下绕过，可在 Chromium 启动参数中添加 `--autoplay-policy=no-user-gesture-required`。

## 配置持久化

`narrate_config`（讲述模式配置）会持久化到 `RD_DATA_DIR/narrate_config.json`：

- **写入时机**：每次 `POST /api/config` 后自动保存
- **写入方式**：原子写入（temp → fsync → rename），断电安全
- **启动行为**：从磁盘加载配置，如果 `autoConnect` + `mcpEnabled` 为 true 则自动重连 MCP
- **Docker**：通过 `xiaozhi-display-data:/data` named volume 持久化

| 数据类型 | 是否持久化 | 说明 |
|---------|-----------|------|
| 上传的图片 | ✅ Docker volume | `/uploads` 目录 |
| narrate_config | ✅ JSON 文件 | `RD_DATA_DIR/narrate_config.json` |
| UI 状态 | ❌ 实时推送 | 设备断开即消失 |

## 文件结构

```
raspberry_pi_display/
├── server.py        # HTTP + WebSocket 服务器
├── audio_player.py  # Opus 音频解码播放（服务端直出）
├── config.py        # 配置管理
├── narrate_mcp.py   # MCP 讲述模式服务
├── mcp_pipe.py      # MCP stdio ↔ WebSocket 桥接
├── web/             # Web 前端
│   └── index.html   # 浏览器 UI（含 Web Audio 播放）
├── data/            # 持久化数据（narrate_config.json）
├── uploads/         # 用户上传的图片
└── assets/          # UI 资源文件
    ├── emojis/      # 表情图片 (PNG/GIF)
    └── backgrounds/ # 背景图片 (PNG)
```

## 自定义资源

### 表情图片

将表情图片放入 `assets/emojis/` 目录，文件名对应表情名称：
- `neutral.png` 或 `neutral.gif` - 中性表情
- `happy.png` 或 `happy.gif` - 开心
- `sad.png` 或 `sad.gif` - 悲伤
- 等等...

浏览器原生支持 GIF 动画。

### 背景图片

背景图片放入 `assets/backgrounds/` 目录，命名规则：
- `bg_light.png` - 亮色主题背景
- `bg_dark.png` - 暗色主题背景

如果没有对应的资源文件，会使用 CSS 渐变背景和简易表情作为替代。

## Kiosk 部署（数字标牌）

在 reComputer/树莓派上做无人值守数字标牌时：

1. **Docker 部署**（推荐）：
   ```bash
   # docker-compose.yml 已配置好持久化卷和环境变量
   docker compose up -d
   ```

2. **音频配置**：设 `RD_LOCAL_AUDIO=1`，音频走 HDMI 直出

3. **浏览器自启动**（Wayland / 新版 RPi OS）：
   ```ini
   # ~/.config/wayfire.ini
   [autostart]
   kiosk = chromium-browser --kiosk --autoplay-policy=no-user-gesture-required http://localhost:8765
   ```

4. **浏览器自启动**（X11 / 旧版 RPi OS）：
   ```bash
   # ~/.config/lxsession/LXDE-pi/autostart
   @chromium-browser --kiosk --autoplay-policy=no-user-gesture-required http://localhost:8765
   ```

5. **Chrome 企业策略**（最可靠的 autoplay 解除方式）：
   ```bash
   sudo mkdir -p /etc/chromium-browser/policies/managed/
   echo '{"AutoplayAllowed": true}' | sudo tee /etc/chromium-browser/policies/managed/autoplay.json
   ```
