# Xiaozhi Remote Display Server
# Base image: Python 3.12 on Debian Bookworm (slim)
FROM python:3.12-slim-bookworm

LABEL org.opencontainers.image.title="Xiaozhi Remote Display"
LABEL org.opencontainers.image.description="Remote display server for Xiaozhi ESP32/SenseCAP Watcher"
LABEL org.opencontainers.image.vendor="Seeed Studio"
LABEL org.opencontainers.image.version="0.2.0"

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install system dependencies
# - build-essential: GCC compiler for building Python extensions
# - portaudio19-dev: Audio I/O library for PyAudio
# - libopus-dev: Opus codec library for audio decoding
# - alsa-utils: ALSA sound utilities for testing
# - pulseaudio-utils: PulseAudio CLI tools (paplay, pactl)
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    portaudio19-dev \
    libopus-dev \
    alsa-utils \
    pulseaudio-utils \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy only requirements first (for better layer caching)
COPY requirements.txt pyproject.toml ./

# Install Python dependencies
RUN pip install --no-cache-dir \
    aiohttp>=3.9.0 \
    opuslib>=3.0.1 \
    pyaudio>=0.2.13 \
    zeroconf>=0.131.0

# Copy application code
COPY server.py audio_player.py config.py ./
COPY web/ ./web/
COPY assets/ ./assets/

# Expose WebSocket/HTTP port
EXPOSE 8765

# Default environment variables
ENV RD_HOST=0.0.0.0
ENV RD_PORT=8765
ENV RD_NO_AUDIO=0
ENV RD_DEVICE_NAME="Xiaozhi Display"

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:8765/ || exit 1

# Run the server
CMD ["python", "server.py"]
