"""Audio bridge between Reachy Mini and Xiaozhi server.

Handles:
- Capturing audio from Reachy Mini microphone
- OPUS encoding and sending to xiaozhi server
- Receiving OPUS audio from server (TTS)
- OPUS decoding and playing through Reachy Mini speaker
"""

import asyncio
import logging
import struct
import threading
import time
from collections import deque
from typing import Optional

import numpy as np

try:
    import opuslib
except ImportError:
    opuslib = None

from .config import AudioConfig

logger = logging.getLogger(__name__)


class OpusCodec:
    """OPUS encoder/decoder wrapper."""

    def __init__(self, config: AudioConfig):
        self._config = config
        if opuslib is None:
            raise ImportError(
                "opuslib is required for OPUS encoding/decoding. "
                "Install with: pip install opuslib"
            )

        # Encoder: mic sample rate -> OPUS
        self._encoder = opuslib.Encoder(
            fs=config.input_sample_rate,
            channels=config.channels,
            application=opuslib.APPLICATION_VOIP,
        )

        # Decoder: OPUS -> speaker sample rate
        self._decoder = opuslib.Decoder(
            fs=config.output_sample_rate,
            channels=config.channels,
        )

        # Frame size in samples for encoding (input sample rate * frame duration)
        self._encode_frame_size = int(
            config.input_sample_rate * config.opus_frame_duration_ms / 1000
        )
        # Frame size for decoding (output sample rate * frame duration)
        self._decode_frame_size = int(
            config.output_sample_rate * config.opus_frame_duration_ms / 1000
        )

    @property
    def encode_frame_size(self) -> int:
        return self._encode_frame_size

    @property
    def decode_frame_size(self) -> int:
        return self._decode_frame_size

    def encode(self, pcm_data: np.ndarray) -> bytes:
        """Encode PCM int16 samples to OPUS."""
        pcm_bytes = pcm_data.astype(np.int16).tobytes()
        return self._encoder.encode(pcm_bytes, self._encode_frame_size)

    def decode(self, opus_data: bytes) -> np.ndarray:
        """Decode OPUS data to PCM float32 samples."""
        pcm_bytes = self._decoder.decode(opus_data, self._decode_frame_size)
        pcm_int16 = np.frombuffer(pcm_bytes, dtype=np.int16)
        return pcm_int16.astype(np.float32) / 32768.0


class AudioBridge:
    """Bridges Reachy Mini audio I/O with the xiaozhi OPUS audio stream."""

    def __init__(self, config: AudioConfig):
        self._config = config
        self._codec = OpusCodec(config)
        self._running = False

        # Buffers
        self._capture_buffer = np.array([], dtype=np.float32)
        self._playback_queue: deque[np.ndarray] = deque(maxlen=200)

        # Async queue for sending encoded audio to the protocol client
        self._send_queue: asyncio.Queue[bytes] = asyncio.Queue(maxsize=100)

    @property
    def send_queue(self) -> asyncio.Queue:
        """Queue of OPUS frames ready to send to the server."""
        return self._send_queue

    def start(self):
        self._running = True

    def stop(self):
        self._running = False
        # Clear buffers
        self._capture_buffer = np.array([], dtype=np.float32)
        self._playback_queue.clear()

    def feed_captured_audio(self, samples: np.ndarray):
        """Feed raw PCM samples from Reachy Mini microphone.

        Accumulates samples and encodes full OPUS frames, placing
        them in the send queue for the protocol client.
        """
        if not self._running:
            return

        # Convert to float32 if needed, normalize to [-1, 1]
        if samples.dtype != np.float32:
            samples = samples.astype(np.float32)
        if np.max(np.abs(samples)) > 1.0:
            samples = samples / 32768.0

        self._capture_buffer = np.concatenate([self._capture_buffer, samples])
        frame_size = self._codec.encode_frame_size

        while len(self._capture_buffer) >= frame_size:
            frame = self._capture_buffer[:frame_size]
            self._capture_buffer = self._capture_buffer[frame_size:]

            # Convert to int16 for OPUS encoding
            frame_int16 = (frame * 32767).astype(np.int16)
            try:
                opus_data = self._codec.encode(frame_int16)
                try:
                    self._send_queue.put_nowait(opus_data)
                except asyncio.QueueFull:
                    logger.warning("Send queue full, dropping frame")
            except Exception as e:
                logger.error(f"OPUS encode error: {e}")

    def on_server_audio(self, opus_data: bytes):
        """Handle incoming OPUS audio from the server (TTS).

        Decodes and queues PCM samples for playback on Reachy Mini speaker.
        """
        try:
            pcm_samples = self._codec.decode(opus_data)
            self._playback_queue.append(pcm_samples)
        except Exception as e:
            logger.error(f"OPUS decode error: {e}")

    def get_playback_samples(self) -> Optional[np.ndarray]:
        """Get the next chunk of decoded PCM samples for playback.

        Returns None if no samples are available.
        """
        if not self._playback_queue:
            return None
        return self._playback_queue.popleft()

    def has_playback_data(self) -> bool:
        """Check if there are samples waiting for playback."""
        return len(self._playback_queue) > 0

    def drain_playback(self) -> np.ndarray:
        """Get all remaining playback samples as a single array."""
        if not self._playback_queue:
            return np.array([], dtype=np.float32)
        chunks = list(self._playback_queue)
        self._playback_queue.clear()
        return np.concatenate(chunks)
