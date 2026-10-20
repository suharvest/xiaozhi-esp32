"""
Audio Player - PCM and Opus audio playback via PyAudio
"""

import logging
from queue import Queue, Empty
from threading import Thread, Event

logger = logging.getLogger(__name__)

# Try to import optional dependencies
try:
    import pyaudio
    PYAUDIO_AVAILABLE = True
except ImportError:
    PYAUDIO_AVAILABLE = False
    logger.warning("PyAudio not available, audio playback disabled")

try:
    import opuslib
    OPUSLIB_AVAILABLE = True
except ImportError:
    OPUSLIB_AVAILABLE = False
    logger.info("opuslib not available, Opus decoding disabled (PCM still works)")


class AudioPlayer:
    def __init__(self, sample_rate: int = 24000, channels: int = 1, frame_duration: int = 60):
        self.sample_rate = sample_rate
        self.channels = channels
        self.frame_duration = frame_duration
        self.frame_size = sample_rate * frame_duration // 1000

        self.running = True
        # Larger queue for smoother playback
        self.audio_queue: Queue = Queue(maxsize=100)

        # Pre-buffer control
        self.prebuffer_count = 3  # Wait for N packets before starting playback
        self.prebuffer_ready = Event()

        if not PYAUDIO_AVAILABLE:
            logger.warning("Audio playback not available (PyAudio missing)")
            self.enabled = False
            return

        self.enabled = True

        # Initialize Opus decoder (optional, for legacy support)
        self.decoder = None
        if OPUSLIB_AVAILABLE:
            try:
                self.decoder = opuslib.Decoder(sample_rate, channels)
            except Exception as e:
                logger.warning(f"Failed to initialize Opus decoder: {e}")

        # Initialize PyAudio with larger buffer
        self.pa = pyaudio.PyAudio()
        # Use larger buffer (4x frame size) to prevent underruns
        buffer_size = self.frame_size * 4
        self.stream = self.pa.open(
            format=pyaudio.paInt16,
            channels=channels,
            rate=sample_rate,
            output=True,
            frames_per_buffer=buffer_size
        )

        # Start playback thread
        self.play_thread = Thread(target=self._play_loop, daemon=True)
        self.play_thread.start()

        logger.info(f"Audio player started: {sample_rate}Hz, buffer={buffer_size} samples")

    def play_pcm(self, pcm_data: bytes, sample_rate: int):
        """Play raw PCM data (non-blocking)"""
        if not self.enabled:
            return

        # Reinit stream if sample rate changed
        if sample_rate != self.sample_rate:
            self._reinit_stream(sample_rate)

        # Put in queue (drop oldest if full to prevent latency buildup)
        if self.audio_queue.full():
            try:
                self.audio_queue.get_nowait()
            except Empty:
                pass

        try:
            self.audio_queue.put_nowait(pcm_data)
            # Signal that we have enough data to start playing
            if not self.prebuffer_ready.is_set() and self.audio_queue.qsize() >= self.prebuffer_count:
                self.prebuffer_ready.set()
                logger.debug("Pre-buffer ready, starting playback")
        except Exception:
            pass

    def decode_opus(self, opus_data: bytes, sample_rate: int, frame_duration: int) -> bytes:
        """Decode Opus to PCM bytes (for forwarding to browsers)"""
        if not self.decoder:
            return None

        # Reinit decoder if sample rate changed
        if sample_rate != self.sample_rate:
            self._reinit(sample_rate, frame_duration)

        try:
            return self.decoder.decode(opus_data, self.frame_size)
        except Exception as e:
            logger.error(f"Failed to decode Opus: {e}")
            return None

    def play_opus(self, opus_data: bytes, sample_rate: int, frame_duration: int):
        """Play Opus data (non-blocking) - legacy support"""
        pcm = self.decode_opus(opus_data, sample_rate, frame_duration)
        if pcm:
            self.play_pcm(pcm, sample_rate)

    def _reinit_stream(self, sample_rate: int):
        """Reinitialize audio stream with new sample rate"""
        self.sample_rate = sample_rate
        self.frame_size = sample_rate * self.frame_duration // 1000
        buffer_size = self.frame_size * 4

        # Reset prebuffer
        self.prebuffer_ready.clear()

        # Reopen audio stream
        self.stream.close()
        self.stream = self.pa.open(
            format=pyaudio.paInt16,
            channels=self.channels,
            rate=sample_rate,
            output=True,
            frames_per_buffer=buffer_size
        )
        logger.info(f"Audio stream reinitialized: {sample_rate}Hz, buffer={buffer_size}")

    def _reinit(self, sample_rate: int, frame_duration: int):
        """Reinitialize decoder and stream with new sample rate"""
        self.frame_duration = frame_duration

        if self.decoder and OPUSLIB_AVAILABLE:
            self.decoder = opuslib.Decoder(sample_rate, self.channels)

        self._reinit_stream(sample_rate)

    def _play_loop(self):
        """Playback loop with pre-buffering"""
        while self.running:
            try:
                # Wait for pre-buffer before starting
                if not self.prebuffer_ready.wait(timeout=0.1):
                    continue

                # Get audio data with timeout
                pcm = self.audio_queue.get(timeout=0.05)
                self.stream.write(pcm)

                # If queue is empty, reset prebuffer to wait for more data
                if self.audio_queue.empty():
                    self.prebuffer_ready.clear()

            except Empty:
                # Queue empty, reset prebuffer
                if self.prebuffer_ready.is_set():
                    self.prebuffer_ready.clear()
            except Exception as e:
                logger.error(f"Playback error: {e}")

    def close(self):
        """Close player"""
        self.running = False
        self.prebuffer_ready.set()  # Unblock wait
        if hasattr(self, 'play_thread') and self.play_thread.is_alive():
            self.play_thread.join(timeout=1.0)
        if hasattr(self, 'stream'):
            self.stream.close()
        if hasattr(self, 'pa'):
            self.pa.terminate()
        logger.info("Audio player stopped")
