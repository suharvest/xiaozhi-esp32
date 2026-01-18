"""
Remote Display Server Configuration
"""

import os

class Config:
    # Server settings
    HOST = os.getenv("RD_HOST", "0.0.0.0")
    PORT = int(os.getenv("RD_PORT", "8765"))

    # Device name for mDNS service discovery
    # Set RD_DEVICE_NAME environment variable to customize
    # Example: RD_DEVICE_NAME="客厅显示器" python server.py
    DEVICE_NAME = os.getenv("RD_DEVICE_NAME", "Raspberry Pi Display")

    # Screen settings
    SCREEN_WIDTH = 412
    SCREEN_HEIGHT = 412
    DISPLAY_SCALE = float(os.getenv("RD_SCALE", "1.5"))
    FULLSCREEN = os.getenv("RD_FULLSCREEN", "0") == "1"

    # Audio settings
    AUDIO_SAMPLE_RATE = 24000
    AUDIO_CHANNELS = 1
    AUDIO_FRAME_DURATION = 60  # ms

    # Debug
    DEBUG = os.getenv("RD_DEBUG", "0") == "1"
