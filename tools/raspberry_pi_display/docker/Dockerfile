# Xiaozhi Remote Display Server
# Multi-stage build to minimize final image size

# ---- Stage 1: Build Python wheels ----
FROM python:3.12-slim-bookworm AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Install build-time dependencies only
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    portaudio19-dev \
    libopus-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY requirements.txt ./

# Build wheels so the runtime stage doesn't need a compiler
RUN pip wheel --no-cache-dir --wheel-dir /wheels -r requirements.txt

# ---- Stage 2: Runtime ----
FROM python:3.12-slim-bookworm

LABEL org.opencontainers.image.title="Xiaozhi Remote Display"
LABEL org.opencontainers.image.description="Remote display server for Xiaozhi ESP32/SenseCAP Watcher"
LABEL org.opencontainers.image.vendor="Seeed Studio"
LABEL org.opencontainers.image.version="0.3.1"

ENV DEBIAN_FRONTEND=noninteractive

# Install runtime-only system libraries (no -dev packages, no build-essential)
RUN apt-get update && apt-get install -y --no-install-recommends \
    libportaudio2 \
    libopus0 \
    pulseaudio-utils \
    libpulse0 \
    libasound2-plugins \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Configure ALSA to use PulseAudio as default output
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

WORKDIR /app

# Install pre-built wheels from builder stage (no compilation needed)
COPY --from=builder /wheels /wheels
COPY requirements.txt ./
RUN pip install --no-cache-dir --no-index --find-links=/wheels -r requirements.txt \
    && rm -rf /wheels

# Copy application code
COPY server.py audio_player.py config.py mcp_pipe.py narrate_mcp.py ./
COPY web/ ./web/
COPY assets/emojis/ ./assets/emojis/
COPY assets/backgrounds/ ./assets/backgrounds/

EXPOSE 8765

ENV RD_HOST=0.0.0.0
ENV RD_PORT=8765
ENV RD_LOCAL_AUDIO=0
ENV RD_DEVICE_NAME="Xiaozhi Display"

HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:8765/ || exit 1

CMD ["python", "server.py"]
