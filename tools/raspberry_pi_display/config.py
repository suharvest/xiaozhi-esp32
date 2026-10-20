"""
Remote Display Server Configuration
"""

import os

class Config:
    # Server settings
    HOST = os.getenv("RD_HOST", "0.0.0.0")
    PORT = int(os.getenv("RD_PORT", "8765"))

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
