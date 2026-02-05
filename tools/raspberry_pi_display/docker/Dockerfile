# Xiaozhi Remote Display Server
# Base image: Python 3.12 on Debian Bookworm (slim)
FROM python:3.12-slim-bookworm

LABEL org.opencontainers.image.title="Xiaozhi Remote Display"
LABEL org.opencontainers.image.description="Remote display server for Xiaozhi ESP32/SenseCAP Watcher"
LABEL org.opencontainers.image.vendor="Seeed Studio"
LABEL org.opencontainers.image.version="0.2.2"

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install system dependencies
# - build-essential: GCC compiler for building Python extensions
# - portaudio19-dev: Audio I/O library for PyAudio
# - libopus-dev: Opus codec library for audio decoding
# - alsa-utils: ALSA sound utilities for testing
# - pulseaudio-utils: PulseAudio CLI tools (paplay, pactl)
# - libpulse0: PulseAudio client library (for PortAudio PulseAudio backend)
# - libasound2-plugins: ALSA plugins including PulseAudio plugin
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    portaudio19-dev \
    libopus-dev \
    alsa-utils \
    pulseaudio-utils \
    libpulse0 \
    libasound2-plugins \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Configure ALSA to use PulseAudio as default output
# This makes PyAudio/PortAudio route audio through PulseAudio/PipeWire
RUN mkdir -p /etc/alsa && echo '\
pcm.!default {\n\
    type pulse\n\
    fallback "sysdefault"\n\
    hint {\n\
        show on\n\
        description "Default ALSA Output (PulseAudio)"\n\
    }\n\
}\n\
ctl.!default {\n\
    type pulse\n\
    fallback "sysdefault"\n\
}\n' > /etc/alsa/conf.d/99-pulse.conf

# Set working directory
WORKDIR /app

# Copy requirements first (for better layer caching)
COPY requirements.txt ./

# Install Python dependencies
RUN pip install --no-cache-dir -r requirements.txt

# Copy application code
COPY server.py audio_player.py config.py ./
COPY web/ ./web/
COPY assets/emojis/ ./assets/emojis/
COPY assets/backgrounds/ ./assets/backgrounds/

# Expose WebSocket/HTTP port
EXPOSE 8765

# Default environment variables
ENV RD_HOST=0.0.0.0
ENV RD_PORT=8765
# Local audio disabled by default, audio plays via browser
# Set RD_LOCAL_AUDIO=1 to enable server-side audio output (HDMI/speaker)
ENV RD_LOCAL_AUDIO=0
ENV RD_DEVICE_NAME="Xiaozhi Display"

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:8765/ || exit 1

# Run the server
CMD ["python", "server.py"]
