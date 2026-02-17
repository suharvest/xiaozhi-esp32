"""Configuration management for the bridge."""

import os
from dataclasses import dataclass, field
from typing import Optional

import yaml


@dataclass
class XiaozhiServerConfig:
    """Xiaozhi server connection settings (WebSocket mode)."""
    websocket_url: str = "ws://localhost:9005/xiaozhi/v1/"
    token: str = ""
    protocol_version: int = 1
    device_id: str = "reachy-mini-bridge-001"
    client_id: str = "reachy-mini-bridge"


@dataclass
class MqttServerConfig:
    """Xiaozhi server connection settings (MQTT+UDP mode)."""
    endpoint: str = ""         # MQTT broker host:port (e.g. "mqtt.example.com:8883")
    client_id: str = ""
    username: str = ""
    password: str = ""
    publish_topic: str = ""    # Topic to publish messages to
    subscribe_topic: str = ""  # Topic to subscribe for incoming messages
    keepalive: int = 240       # MQTT keep-alive interval in seconds


@dataclass
class AudioConfig:
    """Audio pipeline settings."""
    input_sample_rate: int = 16000
    output_sample_rate: int = 24000
    opus_frame_duration_ms: int = 60
    channels: int = 1
    media_backend: str = "default_no_video"


@dataclass
class MotionConfig:
    """Robot motion/expression settings."""
    enable_emotions: bool = True
    enable_head_tracking: bool = False
    head_tracking_max_yaw: float = 30.0
    head_tracking_smoothing: float = 0.3
    head_tracking_poll_interval: float = 0.1
    head_tracking_no_speech_timeout: float = 3.0
    emotion_intensity: float = 0.7
    idle_animation: bool = True
    idle_animation_interval: float = 5.0


@dataclass
class WakeWordConfig:
    """Wake word detection settings."""
    enabled: bool = False
    # If disabled, use button (antenna press) or always-on listening
    mode: str = "always_on"  # "always_on", "antenna_press", "wake_word"
    keyword: str = "你好小智"
    # Path to sherpa-onnx keyword spotting model directory
    model_dir: str = ""
    # Detection sensitivity (0.0 - 1.0, higher = more sensitive but more false positives)
    sensitivity: float = 0.5


@dataclass
class BridgeConfig:
    """Top-level bridge configuration."""
    # Protocol selection: "mqtt" (MQTT+UDP, preferred) or "websocket"
    protocol: str = "websocket"
    server: XiaozhiServerConfig = field(default_factory=XiaozhiServerConfig)
    mqtt: MqttServerConfig = field(default_factory=MqttServerConfig)
    audio: AudioConfig = field(default_factory=AudioConfig)
    motion: MotionConfig = field(default_factory=MotionConfig)
    wake_word: WakeWordConfig = field(default_factory=WakeWordConfig)
    log_level: str = "INFO"

    @classmethod
    def from_yaml(cls, path: str) -> "BridgeConfig":
        """Load configuration from a YAML file."""
        if not os.path.exists(path):
            return cls()
        with open(path, "r") as f:
            data = yaml.safe_load(f) or {}
        config = cls()
        if "protocol" in data:
            config.protocol = data["protocol"]
        if "server" in data:
            for k, v in data["server"].items():
                if hasattr(config.server, k):
                    setattr(config.server, k, v)
        if "mqtt" in data:
            for k, v in data["mqtt"].items():
                if hasattr(config.mqtt, k):
                    setattr(config.mqtt, k, v)
        if "audio" in data:
            for k, v in data["audio"].items():
                if hasattr(config.audio, k):
                    setattr(config.audio, k, v)
        if "motion" in data:
            for k, v in data["motion"].items():
                if hasattr(config.motion, k):
                    setattr(config.motion, k, v)
        if "wake_word" in data:
            for k, v in data["wake_word"].items():
                if hasattr(config.wake_word, k):
                    setattr(config.wake_word, k, v)
        if "log_level" in data:
            config.log_level = data["log_level"]

        # Auto-detect protocol: if mqtt endpoint is set, prefer MQTT
        if config.mqtt.endpoint and config.protocol != "websocket":
            config.protocol = "mqtt"

        return config

    def to_yaml(self, path: str) -> None:
        """Save configuration to a YAML file."""
        from dataclasses import asdict
        with open(path, "w") as f:
            yaml.dump(asdict(self), f, default_flow_style=False)
